#include "duckdb/execution/index/fixed_size_allocator.hpp"

#include "duckdb/storage/metadata/metadata_reader.hpp"

namespace duckdb {

constexpr idx_t FixedSizeAllocator::BASE[];
constexpr uint8_t FixedSizeAllocator::SHIFT[];

FixedSizeAllocator::FixedSizeAllocator(const idx_t segment_size, BlockManager &block_manager)
    : block_manager(block_manager), buffer_manager(block_manager.buffer_manager),
      metadata_manager(block_manager.GetMetadataManager()), segment_size(segment_size), total_segment_count(0) {

	if (segment_size > Storage::BLOCK_SIZE - sizeof(validity_t)) {
		throw InternalException("The maximum segment size of fixed-size allocators is " +
		                        to_string(Storage::BLOCK_SIZE - sizeof(validity_t)));
	}

	// calculate how many segments fit into one buffer (available_segments_per_buffer)

	idx_t bits_per_value = sizeof(validity_t) * 8;
	idx_t byte_count = 0;

	bitmask_count = 0;
	available_segments_per_buffer = 0;

	while (byte_count < Storage::BLOCK_SIZE) {
		if (!bitmask_count || (bitmask_count * bits_per_value) % available_segments_per_buffer == 0) {
			// we need to add another validity_t value to the bitmask, to allow storing another
			// bits_per_value segments on a buffer
			bitmask_count++;
			byte_count += sizeof(validity_t);
		}

		auto remaining_bytes = Storage::BLOCK_SIZE - byte_count;
		auto remaining_segments = MinValue(remaining_bytes / segment_size, bits_per_value);

		if (remaining_segments == 0) {
			break;
		}

		available_segments_per_buffer += remaining_segments;
		byte_count += remaining_segments * segment_size;
	}

	bitmask_offset = bitmask_count * sizeof(validity_t);
}

IndexPointer FixedSizeAllocator::New() {

	// no more segments available
	if (buffers_with_free_space.empty()) {

		// add a new buffer
		auto buffer_id = GetAvailableBufferId();
		FixedSizeBuffer new_buffer(block_manager);
		buffers.insert(make_pair(buffer_id, std::move(new_buffer)));
		buffers_with_free_space.insert(buffer_id);

		// set the bitmask
		D_ASSERT(buffers.find(buffer_id) != buffers.end());
		auto &buffer = buffers.find(buffer_id)->second;
		ValidityMask mask(reinterpret_cast<validity_t *>(buffer.Get()));
		mask.SetAllValid(available_segments_per_buffer);
	}

	// return a pointer
	D_ASSERT(!buffers_with_free_space.empty());
	auto buffer_id = uint32_t(*buffers_with_free_space.begin());

	D_ASSERT(buffers.find(buffer_id) != buffers.end());
	auto &buffer = buffers.find(buffer_id)->second;
	auto bitmask_ptr = reinterpret_cast<validity_t *>(buffer.Get());
	ValidityMask mask(bitmask_ptr);
	auto offset = GetOffset(mask, buffer.segment_count);

	total_segment_count++;
	buffer.segment_count++;
	if (buffer.segment_count == available_segments_per_buffer) {
		buffers_with_free_space.erase(buffer_id);
	}

	return IndexPointer(buffer_id, offset);
}

void FixedSizeAllocator::Free(const IndexPointer ptr) {

	auto buffer_id = ptr.GetBufferId();
	auto offset = ptr.GetOffset();

	D_ASSERT(buffers.find(buffer_id) != buffers.end());
	auto &buffer = buffers.find(buffer_id)->second;

	auto bitmask_ptr = reinterpret_cast<validity_t *>(buffer.Get());
	ValidityMask mask(bitmask_ptr);
	D_ASSERT(!mask.RowIsValid(offset));
	mask.SetValid(offset);

	D_ASSERT(total_segment_count > 0);
	D_ASSERT(buffer.segment_count > 0);

	// adjust the allocator fields
	buffers_with_free_space.insert(buffer_id);
	total_segment_count--;
	buffer.segment_count--;
}

void FixedSizeAllocator::Reset() {
	for (auto &buffer : buffers) {
		buffer.second.Destroy();
	}
	buffers.clear();
	buffers_with_free_space.clear();
	total_segment_count = 0;
}

idx_t FixedSizeAllocator::GetMemoryUsage() const {
	idx_t memory_usage = 0;
	for (auto &buffer : buffers) {
		if (buffer.second.InMemory()) {
			memory_usage += Storage::BLOCK_SIZE;
		}
	}
	return memory_usage;
}

idx_t FixedSizeAllocator::GetUpperBoundBufferId() const {
	idx_t upper_bound_id = 0;
	for (auto &buffer : buffers) {
		if (buffer.first >= upper_bound_id) {
			upper_bound_id = buffer.first + 1;
		}
	}
	return upper_bound_id;
}

void FixedSizeAllocator::Merge(FixedSizeAllocator &other) {

	D_ASSERT(segment_size == other.segment_size);

	// remember the buffer count and merge the buffers
	idx_t upper_bound_id = GetUpperBoundBufferId();
	for (auto &buffer : other.buffers) {
		buffers.insert(make_pair(buffer.first + upper_bound_id, std::move(buffer.second)));
	}
	other.buffers.clear();

	// merge the buffers with free spaces
	for (auto &buffer_id : other.buffers_with_free_space) {
		buffers_with_free_space.insert(buffer_id + upper_bound_id);
	}
	other.buffers_with_free_space.clear();

	// add the total allocations
	total_segment_count += other.total_segment_count;
}

bool FixedSizeAllocator::InitializeVacuum() {

	// NOTE: we do not vacuum buffers that are not in memory. We might consider changing this
	// in the future, although buffers on disk should almost never be eligible for a vacuum

	if (total_segment_count == 0) {
		Reset();
		return false;
	}

	multimap<idx_t, idx_t> temporary_vacuum_buffers;
	D_ASSERT(vacuum_buffers.empty());
	idx_t available_segments_in_memory = 0;

	for (auto &buffer : buffers) {
		buffer.second.vacuum = false;
		if (buffer.second.InMemory()) {
			auto available_segments_in_buffer = available_segments_per_buffer - buffer.second.segment_count;
			available_segments_in_memory += available_segments_in_buffer;
			temporary_vacuum_buffers.emplace(available_segments_in_buffer, buffer.first);
		}
	}

	// no buffers in memory
	if (temporary_vacuum_buffers.empty()) {
		return false;
	}

	auto excess_buffer_count = available_segments_in_memory / available_segments_per_buffer;

	// calculate the vacuum threshold adaptively
	D_ASSERT(excess_buffer_count < temporary_vacuum_buffers.size());
	idx_t memory_usage = GetMemoryUsage();
	idx_t excess_memory_usage = excess_buffer_count * Storage::BLOCK_SIZE;
	auto excess_percentage = double(excess_memory_usage) / double(memory_usage);
	auto threshold = double(VACUUM_THRESHOLD) / 100.0;
	if (excess_percentage < threshold) {
		return false;
	}

	D_ASSERT(excess_buffer_count <= temporary_vacuum_buffers.size());
	D_ASSERT(temporary_vacuum_buffers.size() <= buffers.size());

	// erasing from a multimap, we vacuum the buffers with the most free spaces (least full)
	while (temporary_vacuum_buffers.size() != excess_buffer_count) {
		temporary_vacuum_buffers.erase(temporary_vacuum_buffers.begin());
	}

	// adjust the buffers, and erase all to-be-vacuumed buffers from the available buffer list
	for (auto &vacuum_buffer : temporary_vacuum_buffers) {
		auto buffer_id = vacuum_buffer.second;
		D_ASSERT(buffers.find(buffer_id) != buffers.end());
		buffers.find(buffer_id)->second.vacuum = true;
		buffers_with_free_space.erase(buffer_id);
	}

	for (auto &vacuum_buffer : temporary_vacuum_buffers) {
		vacuum_buffers.insert(vacuum_buffer.second);
	}

	return true;
}

void FixedSizeAllocator::FinalizeVacuum() {

	for (auto &buffer_id : vacuum_buffers) {
		D_ASSERT(buffers.find(buffer_id) != buffers.end());
		auto &buffer = buffers.find(buffer_id)->second;
		D_ASSERT(buffer.InMemory());
		buffer.Destroy();
		buffers.erase(buffer_id);
	}
	vacuum_buffers.clear();
}

IndexPointer FixedSizeAllocator::VacuumPointer(const IndexPointer ptr) {

	// we do not need to adjust the bitmask of the old buffer, because we will free the entire
	// buffer after the vacuum operation

	auto new_ptr = New();
	// new increases the allocation count, we need to counter that here
	total_segment_count--;

	memcpy(Get(new_ptr), Get(ptr), segment_size);
	return new_ptr;
}

BlockPointer FixedSizeAllocator::Serialize(PartialBlockManager &partial_block_manager, MetadataWriter &writer) {

	for (auto &buffer : buffers) {
		ValidityMask mask(reinterpret_cast<validity_t *>(buffer.second.Get()));
		auto max_offset = GetMaxOffset(mask);
		auto allocation_size = max_offset * segment_size + bitmask_offset;
		buffer.second.Serialize(partial_block_manager, allocation_size);
	}

	auto block_pointer = writer.GetBlockPointer();
	writer.Write(segment_size);
	writer.Write(static_cast<idx_t>(buffers.size()));
	writer.Write(static_cast<idx_t>(buffers_with_free_space.size()));

	for (auto &buffer : buffers) {
		writer.Write(buffer.first);
		writer.Write(buffer.second.block_pointer);
		writer.Write(buffer.second.segment_count);
		writer.Write(buffer.second.allocation_size);
	}
	for (auto &buffer_id : buffers_with_free_space) {
		writer.Write(buffer_id);
	}

	return block_pointer;
}

void FixedSizeAllocator::Deserialize(const BlockPointer &block_pointer) {

	MetadataReader reader(metadata_manager, block_pointer);
	segment_size = reader.Read<idx_t>();
	auto buffer_count = reader.Read<idx_t>();
	auto buffers_with_free_space_count = reader.Read<idx_t>();

	total_segment_count = 0;

	for (idx_t i = 0; i < buffer_count; i++) {
		auto buffer_id = reader.Read<idx_t>();
		auto buffer_block_pointer = reader.Read<BlockPointer>();
		auto segment_count = reader.Read<idx_t>();
		auto allocation_size = reader.Read<idx_t>();
		FixedSizeBuffer new_buffer(block_manager, segment_count, allocation_size, buffer_block_pointer);
		buffers.insert(make_pair(buffer_id, std::move(new_buffer)));
		total_segment_count += segment_count;
	}
	for (idx_t i = 0; i < buffers_with_free_space_count; i++) {
		buffers_with_free_space.insert(reader.Read<idx_t>());
	}
}

uint32_t FixedSizeAllocator::GetOffset(ValidityMask &mask, const idx_t segment_count) {

	auto data = mask.GetData();

	// fills up a buffer sequentially before searching for free bits
	if (mask.RowIsValid(segment_count)) {
		mask.SetInvalid(segment_count);
		return segment_count;
	}

	for (idx_t entry_idx = 0; entry_idx < bitmask_count; entry_idx++) {
		// get an entry with free bits
		if (data[entry_idx] == 0) {
			continue;
		}

		// find the position of the free bit
		auto entry = data[entry_idx];
		idx_t first_valid_bit = 0;

		// this loop finds the position of the rightmost set bit in entry and stores it
		// in first_valid_bit
		for (idx_t i = 0; i < 6; i++) {
			// set the left half of the bits of this level to zero and test if the entry is still not zero
			if (entry & BASE[i]) {
				// first valid bit is in the rightmost s[i] bits
				// permanently set the left half of the bits to zero
				entry &= BASE[i];
			} else {
				// first valid bit is in the leftmost s[i] bits
				// shift by s[i] for the next iteration and add s[i] to the position of the rightmost set bit
				entry >>= SHIFT[i];
				first_valid_bit += SHIFT[i];
			}
		}
		D_ASSERT(entry);

		auto prev_bits = entry_idx * sizeof(validity_t) * 8;
		D_ASSERT(mask.RowIsValid(prev_bits + first_valid_bit));
		mask.SetInvalid(prev_bits + first_valid_bit);
		return (prev_bits + first_valid_bit);
	}

	throw InternalException("Invalid bitmask for FixedSizeAllocator");
}

uint32_t FixedSizeAllocator::GetMaxOffset(ValidityMask &mask) {

	// finds the maximum free bit in a bitmask, and adds one to it,
	// so that max_offset * segment_size = allocated_size of this bitmask's buffer

	auto data = mask.GetData();
	uint32_t max_offset = bitmask_count * sizeof(validity_t) * 8;

	auto bits_in_last_entry = available_segments_per_buffer % (sizeof(validity_t) * 8);

	D_ASSERT(bitmask_count > 0);
	for (idx_t i = bitmask_count; i > 0; i--) {

		auto entry = data[i - 1];

		// set all bits after bits_in_last_entry
		if (i == bitmask_count) {
			entry |= ~idx_t(0) << bits_in_last_entry;
		}

		if (entry == ~idx_t(0)) {
			max_offset -= sizeof(validity_t) * 8;
			continue;
		}

		// invert data[entry_idx]
		auto entry_inv = ~entry;
		idx_t first_valid_bit = 0;

		// then find the position of the LEFTMOST set bit
		for (idx_t level = 0; level < 6; level++) {

			// set the right half of the bits of this level to zero and test if the entry is still not zero
			if (entry_inv & ~BASE[level]) {
				// first valid bit is in the leftmost s[level] bits
				// shift by s[level] for the next iteration and add s[level] to the position of the leftmost set bit
				entry_inv >>= SHIFT[level];
				first_valid_bit += SHIFT[level];
			} else {
				// first valid bit is in the rightmost s[level] bits
				// permanently set the left half of the bits to zero
				entry_inv &= BASE[level];
			}
		}
		D_ASSERT(entry_inv);
		max_offset -= sizeof(validity_t) * 8 - first_valid_bit;
		D_ASSERT(!mask.RowIsValid(max_offset));
		return max_offset + 1;
	}

	// there are no allocations in this buffer
	throw InternalException("tried to serialize empty buffer");
}

idx_t FixedSizeAllocator::GetAvailableBufferId() const {
	idx_t buffer_id = buffers.size();
	while (buffers.find(buffer_id) != buffers.end()) {
		D_ASSERT(buffer_id > 0);
		buffer_id--;
	}
	return buffer_id;
}

} // namespace duckdb
