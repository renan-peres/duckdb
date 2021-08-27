//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/set/physical_recursive_cte.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {
class Pipeline;

class PhysicalRecursiveCTE : public PhysicalOperator {
public:
	PhysicalRecursiveCTE(vector<LogicalType> types, bool union_all, unique_ptr<PhysicalOperator> top,
	                     unique_ptr<PhysicalOperator> bottom, idx_t estimated_cardinality);
	~PhysicalRecursiveCTE() override;

	bool union_all;
	std::shared_ptr<ChunkCollection> working_table;
	vector<shared_ptr<Pipeline>> pipelines;

public:
	// void GetChunkInternal(ExecutionContext &context, DataChunk &chunk, OperatorState *state) const override;
	// unique_ptr<OperatorState> GetOperatorState() override;
	// void FinalizeOperatorState(OperatorState &state_p, ExecutionContext &context) override;

private:
	//! Probe Hash Table and eliminate duplicate rows
	idx_t ProbeHT(DataChunk &chunk, OperatorState *state) const;

	void ExecuteRecursivePipelines(ExecutionContext &context) const;
};

} // namespace duckdb
