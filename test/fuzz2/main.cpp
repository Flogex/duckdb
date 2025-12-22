#include "duckdb/common/random_engine.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"

using namespace duckdb;

class StatementGenerator {
public:
	unique_ptr<SQLStatement> GetNext() {
		return create_projection();
	}

private:
	RandomEngine rnd;

	unique_ptr<SelectStatement> create_projection() {
		auto statement = make_uniq<SelectStatement>();
		auto node = make_uniq<SelectNode>();
		TableDescription table("memory", "main", "my_tbl");
		node->from_table = make_uniq_base<TableRef, BaseTableRef>(table);
		auto star_expr = make_uniq_base<ParsedExpression, StarExpression>("my_tbl");
		node->select_list.push_back(std::move(star_expr));
		statement->node = std::move(node);
		return statement;
	}
};


int main() {
	StatementGenerator gen;
	auto statement = gen.GetNext();

	DuckDB db;
	// TODO: Randomized config options
	Connection con(db);
	con.Query("CREATE TABLE my_tbl AS SELECT 42");
	auto res = con.Query(std::move(statement));
	res->Print();
	return 0;
}
