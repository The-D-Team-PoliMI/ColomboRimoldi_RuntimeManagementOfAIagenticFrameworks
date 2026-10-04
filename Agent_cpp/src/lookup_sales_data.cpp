#include "lookup_sales_data.h"
#include "utils.h"
#include <regex>
#include <iostream>
#include <sstream>
#include <filesystem>
#include <fstream>

namespace agent_cpp{

const std::string LookupSalesData::SQL_GENERATION_PROMPT = R"(You are an expert SQL developer specializing in DuckDB queries for data analysis and visualization.

## TASK
Generate a DuckDB SQL query to answer the user's question and provide data optimized for visualization.

## AVAILABLE DATA
{schema_context}

## USER QUESTION
{prompt}

## VISUALIZATION GOAL
{visualization_goal}

## INSTRUCTIONS
1. Analyze the user's question to identify what data is needed
2. Consider the visualization goal to structure the query output appropriately
3. Select appropriate columns from the schema above
4. Use proper SQL syntax for filtering, aggregation, sorting, and joins across tables
5. For DATE columns with pattern matching, CAST to VARCHAR: CAST(date_column AS VARCHAR) LIKE '%2021-11%'
6. Handle NULL values appropriately
7. Use DuckDB-specific functions when beneficial
8. **When using JOINs**: always qualify every column reference with its table alias (e.g. `st.region`, not `region`). In SELECT, GROUP BY, ORDER BY, and WHERE, prefix each column with the correct alias of the table it belongs to. Never reference a column by name alone when multiple tables are in scope.

## QUERY OPTIMIZATION FOR VISUALIZATION
- **For time series plots**: Ensure dates are sorted chronologically, use DATE_TRUNC for proper granularity
- **For bar charts**: Aggregate data by category, order by the metric being compared
- **For scatter plots**: Select two numeric columns that show relationships
- **For trend analysis**: Include time-based grouping (daily, monthly, yearly)
- **General**: Limit result size if needed, ensure clean column names for axis labels

## EXAMPLES

Example 1:
Question: "Show me sales from November 2021"
Visualization: "Monthly sales trend"
Query: SELECT Date, SUM(Revenue) as Total_Revenue FROM sales WHERE CAST(Date AS VARCHAR) LIKE '%2021-11%' GROUP BY Date ORDER BY Date

Example 2:
Question: "What are the top 5 products by total revenue?"
Visualization: "Compare products by revenue"
Query: SELECT Product_Name, SUM(Revenue) as Total_Revenue FROM sales GROUP BY Product_ID, Product_Name ORDER BY Total_Revenue DESC LIMIT 5

Example 3:
Question: "Show monthly total sales for 2021"
Visualization: "Revenue trends over time"
Query: SELECT DATE_TRUNC('month', Date) as Month, SUM(Revenue) as Monthly_Sales FROM sales WHERE EXTRACT(YEAR FROM Date) = 2021 GROUP BY Month ORDER BY Month

Example 4:
Question: "Analyze price vs demand relationship"
Visualization: "Price vs demand correlation"
Query: SELECT Price, Units_Sold FROM sales WHERE Price IS NOT NULL AND Units_Sold IS NOT NULL

Example 5 (multi-table):
Question: "Show total revenue by product category for 2023"
Visualization: "Bar chart of revenue by category"
Schema:
  Table: sales (columns: Sold_Date, SKU_Coded, Total_Sale_Value)
  Table: products (columns: SKU_Coded, Category, Product_Name)
Query: SELECT p.Category, SUM(s.Total_Sale_Value) as Total_Revenue FROM sales s JOIN products p ON s.SKU_Coded = p.SKU_Coded WHERE EXTRACT(YEAR FROM s.Sold_Date) = 2023 GROUP BY p.Category ORDER BY Total_Revenue DESC

## IMPORTANT — "Average of aggregates" pattern
When the question asks for "average monthly [metric]", "average daily [metric]", etc., you MUST:
1. First aggregate raw rows to the desired period (e.g., compute monthly totals per store using SUM and GROUP BY store + month).
2. Then wrap that result in an outer query or subquery and apply AVG to the aggregated values.
Do NOT apply AVG directly to individual transaction/row values — that gives the average transaction size, not the average monthly metric.

Example 6 (two-level aggregation — "average monthly revenue"):
Question: "Compare average monthly revenue between store regions for 2022 and 2023"
Visualization: "Grouped bar chart comparing average monthly revenue per region between 2022 and 2023"
Schema:
  Table: sales (columns: Sold_Date, Store_Number, Total_Sale_Value)
  Table: stores (columns: Store_Number, region)
Query: SELECT st.region, s.yr AS year, ROUND(AVG(s.monthly_rev), 2) AS avg_monthly_revenue FROM (SELECT Store_Number, YEAR(CAST(Sold_Date AS DATE)) AS yr, DATE_TRUNC('month', CAST(Sold_Date AS DATE)) AS month, SUM(Total_Sale_Value) AS monthly_rev FROM sales WHERE YEAR(CAST(Sold_Date AS DATE)) IN (2022, 2023) GROUP BY Store_Number, yr, month) s JOIN stores st ON s.Store_Number = st.Store_Number GROUP BY st.region, s.yr ORDER BY st.region, s.yr

## COMMON MISTAKES TO AVOID
- **NEVER use SUBSTR() or SUBSTRING() directly on a DATE column** — DuckDB DATE columns are not strings.
  WRONG: `WHERE CAST(SUBSTR(Sold_Date, 1, 4) AS INTEGER) = 2021`
  RIGHT: `WHERE YEAR(Sold_Date) = 2021`
- **NEVER use LIKE directly on a DATE column** — cast to VARCHAR first.
  WRONG: `WHERE Sold_Date LIKE '2023%'`
  RIGHT: `WHERE CAST(Sold_Date AS VARCHAR) LIKE '2023%'`
- **NEVER use strptime() on a column that is already DATE type** — it expects a string input.
  WRONG: `CAST(strptime(Sold_Date, '%Y-%m-%d') AS DATE)`
  RIGHT: `Sold_Date` (already DATE, no cast needed)
- **NEVER use strftime(date, format)** — that is SQLite argument order. DuckDB does not support it.
  WRONG: `strftime(Sold_Date, '%Y')`
  RIGHT: `YEAR(Sold_Date)` or `EXTRACT(YEAR FROM Sold_Date)`
- **To extract year from a DATE column**: use `YEAR(date_col)` or `EXTRACT(YEAR FROM date_col)`
- **To extract month from a DATE column**: use `MONTH(date_col)` or `EXTRACT(MONTH FROM date_col)`
- **NEVER use GROUP BY on a binary flag to split a metric** — this produces multiple rows per period instead of one pivoted row. This applies both to grouping on the raw flag column AND to grouping on a CASE WHEN label derived from it.
  WRONG: `SELECT month, CASE WHEN is_active = 1 THEN 'Active' ELSE 'Inactive' END AS status_label, SUM(amount) AS total FROM transactions GROUP BY month, status_label`
  RIGHT: `SELECT month, SUM(CASE WHEN is_active = 1 THEN amount ELSE 0 END) AS active_amount, SUM(CASE WHEN is_active = 0 THEN amount ELSE 0 END) AS inactive_amount FROM transactions GROUP BY month`
- **NEVER put a CASE WHEN expression in SELECT unless it is either in GROUP BY or wrapped in an aggregate function** — any bare CASE WHEN in a grouped query that is not itself aggregated causes a DuckDB error ("not an aggregate function and does not appear in the GROUP BY clause"). This includes year/period label columns like `CASE WHEN YEAR(date)=2022 THEN '2022' ELSE NULL END AS yr_label`.
  WRONG: `SELECT dept_name, CASE WHEN YEAR(hire_date) = 2022 THEN '2022' ELSE NULL END AS yr_label, AVG(salary) AS avg_sal FROM employees GROUP BY dept_name`
  RIGHT: `SELECT dept_name, YEAR(hire_date) AS year, AVG(salary) AS avg_salary FROM employees GROUP BY dept_name, YEAR(hire_date)`
- **NEVER group by a category dimension (e.g. department, region) inside the innermost subquery when computing averages per entity** — you lose per-entity granularity and the AVG becomes wrong. Always aggregate at the lowest entity level first, then join to the category.
  WRONG: `WITH monthly AS (SELECT dept, YEAR(date) AS yr, DATE_TRUNC('month', date) AS mo, SUM(amount) AS total FROM orders JOIN employees ON ... JOIN departments ON ... GROUP BY dept, yr, mo) SELECT dept, yr, AVG(total) FROM monthly GROUP BY dept, yr`
  RIGHT: `WITH monthly AS (SELECT emp_id, YEAR(date) AS yr, DATE_TRUNC('month', date) AS mo, SUM(amount) AS total FROM orders GROUP BY emp_id, yr, mo) SELECT d.dept, m.yr, AVG(m.total) FROM monthly m JOIN employees e ON m.emp_id = e.id JOIN departments d ON e.dept_id = d.id GROUP BY d.dept, m.yr`
- **When comparing independent per-year metrics (averages, sums, counts per year), return results in LONG FORMAT** — one row per (entity, year), with year as a plain GROUP BY column. Do NOT pivot years into separate columns (avg_2022, avg_2023). Use pivot format ONLY when you need both years in the same row to compute a derived cross-year metric (e.g. year-over-year growth rate).
  WRONG: `SELECT channel, AVG(CASE WHEN YEAR(order_date) = 2021 THEN monthly_rev END) AS avg_2021, AVG(CASE WHEN YEAR(order_date) = 2022 THEN monthly_rev END) AS avg_2022 FROM orders GROUP BY channel`
  RIGHT: `SELECT channel, YEAR(order_date) AS year, AVG(monthly_rev) AS avg_monthly_revenue FROM orders GROUP BY channel, YEAR(order_date) ORDER BY channel, year`
- **When the prompt requests a value "as a percentage", always multiply by 100** — dividing alone returns a fraction (e.g. 0.06), not a percentage (e.g. 6.0). Always write `ratio * 100` explicitly.
  WRONG: `SELECT dept, ROUND(SUM(CASE WHEN is_bonus = 1 THEN amount ELSE 0 END) / NULLIF(SUM(amount), 0), 4) AS bonus_share FROM payroll GROUP BY dept`
  RIGHT: `SELECT dept, ROUND(SUM(CASE WHEN is_bonus = 1 THEN amount ELSE 0 END) / NULLIF(SUM(amount), 0) * 100, 2) AS bonus_share_pct FROM payroll GROUP BY dept`
- **The outer query of a subquery can ONLY access columns that the subquery explicitly lists in its SELECT clause** — never reference original table columns or inner aliases from outside the subquery.
  WRONG: `SELECT sub.hire_date, YEAR(sub.hire_date) AS yr, AVG(sub.monthly_total) FROM (SELECT emp_id, YEAR(hire_date) AS yr, SUM(salary) AS monthly_total FROM employees GROUP BY emp_id, yr) sub GROUP BY sub.hire_date, yr` — `sub.hire_date` does not exist in the subquery SELECT
  RIGHT: `SELECT sub.yr AS year, AVG(sub.monthly_total) FROM (SELECT emp_id, YEAR(hire_date) AS yr, SUM(salary) AS monthly_total FROM employees GROUP BY emp_id, yr) sub GROUP BY sub.yr`

## OUTPUT FORMAT
Return ONLY the SQL query as plain text. No explanations. No markdown formatting. No code fences. Just the SQL query.
)";

LookupSalesData::LookupSalesData(std::shared_ptr<agents::Context> ctx, const DatabaseSchema& schema): context_(ctx), schema_(schema){
    if (duckdb_open(nullptr, &db_) == DuckDBError) 
        std::cerr << "[LookupSalesData] Impossibile avviare il database DuckDB in-memory" << std::endl;
    if (duckdb_connect(db_, &con_) == DuckDBError) 
        std::cerr << "[LookupSalesData] Impossibile aprire la connessione DuckDB" << std::endl;
}

LookupSalesData::~LookupSalesData(){
    if (con_) 
        duckdb_disconnect(&con_);
    if (db_) 
        duckdb_close(&db_);
}

// Inizializza il database in memoria
bool LookupSalesData::init_database(){
    for (const auto& table : schema_.tables) {
        std::string query = "CREATE TABLE " + table.name + " AS SELECT * FROM read_parquet('" + table.file_path + "');";
        duckdb_result res;
        if (duckdb_query(con_, query.c_str(), &res) == DuckDBError) {
            std::cerr << "[LookupSalesData] Errore caricamento tabella " << table.name 
                      << ": " << duckdb_result_error(&res) << std::endl;
            duckdb_destroy_result(&res);
            return false;
        }
        duckdb_destroy_result(&res);
    }
    return true;
}

// Estrae la query SQL pulita dall'output dell'LLm
std::string LookupSalesData::clean_sql_output(const std::string& raw_output){
    std::string cleaned = raw_output;

    replace_all(cleaned, "```sql", "");
    replace_all(cleaned, "```", "");

    auto first = cleaned.find_first_not_of(" \n\r\t");
    if(first == std::string::npos) 
        return "";

    auto last = cleaned.find_last_not_of(" \n\r\t");
    return cleaned.substr(first, (last - first + 1));
}

// Converete il risultato della query in una tabella CSV
std::string LookupSalesData::query_result_to_csv(duckdb_result& result){
    std::stringstream ss;
    idx_t col_count = duckdb_column_count(&result);
    idx_t row_count = duckdb_row_count(&result);

    for(idx_t col = 0; col < col_count; ++col){
        const char* name = duckdb_column_name(&result, col);
        ss << (name ? name : "") << (col < col_count - 1 ? "," : "");
    }
    ss << "\n";

    for(idx_t row = 0; row < row_count; ++row){
        for(idx_t col = 0; col < col_count; ++col){
            char* val = duckdb_value_varchar(&result, col, row);
            ss << (val ? val : "") << (col < col_count - 1 ? "," : "");
            if(val) 
                duckdb_free(val);
        }
        ss << "\n";
    }

    std::string csv_content = ss.str();

    namespace fs = std::filesystem;
    fs::create_directories("Output");

    std::ofstream out_file("Output/result.csv");
    if(out_file.is_open()){
        out_file << csv_content;
        out_file.close();
    } else 
        std::cerr << "[LookupSalesData] Errore apertura file in Output/result.csv\n";

    return csv_content;
}

// riceve lo stato corrente della pipeline, genera la query SQL tramite l'LLM, 
// la esegue e aggiorna lo stato con i dati recuperati
JsonObject LookupSalesData::run(JsonObject& state){
    std::string prompt = state.value("prompt", "");
    std::string visualization_goal = state.value("visualization_goal", "");
    if(visualization_goal.empty())
        visualization_goal = prompt;

    std::string formatted_prompt = SQL_GENERATION_PROMPT;
    replace_all(formatted_prompt, "{schema_context}", schema_.get_full_schema_str());
    replace_all(formatted_prompt, "{prompt}", prompt);
    replace_all(formatted_prompt, "{visualization_goal}", visualization_goal);

    try{
        auto llm = context_->getLLM();
        auto response = llm->chat(formatted_prompt);
        std::string raw_llm_response = response.content;

        std::string sql_query = clean_sql_output(raw_llm_response);
        state["sql_query"] = sql_query;
        std::cout << "\nGenerated SQL Query:\n" << sql_query << "\n" << std::endl;

        duckdb_result result;
        if(duckdb_query(con_, sql_query.c_str(), &result) == DuckDBError){
            std::string err = duckdb_result_error(&result) ? duckdb_result_error(&result) : "Unknown DuckDB error";
            std::cerr << "[LookupSalesData] Error executing query: " << err << std::endl;
            state["data"] = "";
            state["error"] = "Error accessing data: " + err;
        } else{
            std::string csv_data = query_result_to_csv(result);
            state["data"] = csv_data;
            std::cout << "Retrieved Data:\n" << csv_data << "\n" << std::endl;
        }
        duckdb_destroy_result(&result);

    } catch (const std::exception& e){
        state["data"] = "";
        state["error"] = std::string("Error during lookup: ") + e.what();
        std::cerr << "[LookupSalesData] Exception: " << e.what() << std::endl;
    }

    return state;
}

} 