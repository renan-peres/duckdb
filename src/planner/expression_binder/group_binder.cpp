#include "planner/expression_binder/group_binder.hpp"

using namespace duckdb;
using namespace std;


GroupBinder::GroupBinder(Binder &binder, ClientContext &context, SelectNode& node) : 
	ExpressionBinder(binder, context, node) {
	
}

BindResult GroupBinder::BindExpression(unique_ptr<Expression> expr) {
	switch(expr->GetExpressionClass()) {
		case ExpressionClass::AGGREGATE:
			return BindResult(move(expr), "GROUP BY clause cannot contain aggregates!");
		case ExpressionClass::WINDOW:
			return BindResult(move(expr), "GROUP clause cannot contain window functions!");
		case ExpressionClass::COLUMN_REF:
			return BindColumnRefExpression(move(expr));
		case ExpressionClass::FUNCTION:
			return BindFunctionExpression(move(expr));
		case ExpressionClass::SUBQUERY:
			return BindSubqueryExpression(move(expr));
		default:
			return BindChildren(move(expr));
	}
}
