import duckdb

sql_query = "SELECT {foo: 'bar'}::JSON::VARCHAR"

print("With duckdb.sql")
duckdb.sql("SELECT {foo: 'bar'}::JSON::VARCHAR")


print("With res.fetchall")
con = duckdb.connect(database = ":memory:")
try:
    relation = con.query(sql_query)
    result = relation.fetchall()
except duckdb.Error as e:
    print("FAIL!!", e)