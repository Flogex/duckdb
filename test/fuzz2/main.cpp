#include "duckdb/common/random_engine.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"

using namespace duckdb;

class StatementGenerator {
public:
	unique_ptr<SQLStatement> GetNext() {
		if (i == 0) {
			i++;
			return create_insert();
		}
		return create_projection();
	}

private:
	int i = 0;
	RandomEngine rnd;

	unique_ptr<SQLStatement> create_insert() {
		auto statement = make_uniq<InsertStatement>();
		TableDescription table("memory", "main", "my_tbl");
		statement->table = "my_tbl";
		statement->columns = {"i"};
		statement->select_statement = make_uniq<SelectStatement>();
		auto select_node = make_uniq<SelectNode>();
		select_node->from_table = make_uniq_base<TableRef, EmptyTableRef>();
		select_node->select_list.push_back(
		    make_uniq_base<ParsedExpression, ConstantExpression>(Value::INTEGER(43)));
		statement->select_statement->node = std::move(select_node);

		return unique_ptr_cast<InsertStatement, SQLStatement>(std::move(statement));
	}

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

	DuckDB db;
	// TODO: Randomized config options
	Connection con(db);
	con.Query("CREATE TABLE my_tbl AS SELECT 42 as i, 'hello' as s");

	for (auto i = 0; i < 2; i++) {
		auto statement = gen.GetNext();
		auto res = con.Query(std::move(statement));
		res->Print();
	}

	return 0;
}
