#include "steps.h"
#include "utils.h"
#include <iostream>
#include <sstream>
#include <fstream>
#include <numeric>
#include <algorithm>
#include <array>
#include <cstdio>
#include <iomanip>

namespace agent_cpp{

std::string extract_step_output(const std::string& step_name, const JsonObject& result){
    if(step_name == "lookup_sales_data")
        return result.value("sql_query", "");
    else if(step_name == "decide_tool")
        return result.value("tool_choice", "");
    else{
        // ritorna l'ultimo output generato da AnalyzeData o CreateVisualization, ovvero la descrizione dei dati oppure lo script python del grafo
        if(result.contains("answer") && result["answer"].is_array() && !result["answer"].empty())
            return result["answer"].back().get<std::string>();
        return "";
    }
}

std::string format_cot_prompt(const std::string& base_prompt, const JsonObject& state){
    if(!state.contains("_cot_previous_response"))
        return base_prompt;

    std::string prev = state.value("_cot_previous_response", "");
    std::string err  = state.value("_cot_execution_error", "");

    std::ostringstream oss;
    oss << base_prompt;

    if(!err.empty()){
        oss << "\n\n## ITERATIVE REFINEMENT — EXECUTION ERROR\n"
            << "Your previous attempt produced the following response:\n"
            << "---\n" << prev << "\n---\n"
            << "When executed, it raised the following error:\n"
            << "---\n" << err << "\n---\n"
            << "You MUST fix this error. Output only the corrected response with no meta-commentary.\n";
    } else{
        oss << "\n\n## ITERATIVE REFINEMENT\n"
            << "Your previous attempt produced the following response:\n"
            << "---\n" << prev << "\n---\n"
            << "Carefully review your previous response.\n"
            << "- If it is correct and complete, reproduce it exactly (same content, same format).\n"
            << "- If you identify errors or improvements, output a revised version.\n"
            << "Output only the final response with no meta-commentary.\n";
    }
    return oss.str();
}


static const std::string SQL_GENERATION_PROMPT = R"(You are an expert SQL developer specializing in DuckDB queries for data analysis and visualization.

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


/**
 * @brief Rimuove i blocchi markdown e gli spazi superflui dalla query generata dall'LLM.
 */
static std::string clean_sql_output(const std::string& raw_output){
    std::string cleaned = raw_output;

    replace_all(cleaned, "```sql", "");
    replace_all(cleaned, "```", "");

    auto first = cleaned.find_first_not_of(" \n\r\t");
    if(first == std::string::npos) 
        return "";

    auto last = cleaned.find_last_not_of(" \n\r\t");
    return cleaned.substr(first, (last - first + 1));
}

JsonObject LookupSalesData(JsonObject& state, std::shared_ptr<agents::Context> context, const DatabaseSchema& schema, duckdb_connection con){
    std::string prompt = state.value("prompt", "");
    std::string visualization_goal = state.value("visualization_goal", "");
    if(visualization_goal.empty())
        visualization_goal = prompt;

    std::string formatted_prompt = SQL_GENERATION_PROMPT;
    replace_all(formatted_prompt, "{schema_context}", schema.get_full_schema_str());
    replace_all(formatted_prompt, "{prompt}", prompt);
    replace_all(formatted_prompt, "{visualization_goal}", visualization_goal);

    try{
        std::string formatted_cot_prompt = format_cot_prompt(formatted_prompt, state);
        auto llm = context->getLLM();
        auto response = llm->chat(formatted_cot_prompt);
        std::string raw_llm_response = response.content;

        std::string sql_query = clean_sql_output(raw_llm_response);
        state["sql_query"] = sql_query;
        std::cout << "\nGenerated SQL Query:\n" << sql_query << "\n" << std::endl;

        duckdb_result result;
        if(duckdb_query(con, sql_query.c_str(), &result) == DuckDBError){
            std::string err = duckdb_result_error(&result) ? duckdb_result_error(&result) : "Unknown DuckDB error";
            std::cerr << "[LookupSalesData] Error executing query: " << err << std::endl;
            state["data"] = "";
            state["error"] = "Error accessing data: " + err;
        } else{
            std::string csv_data = query_result_to_csv(result);
            state["data"] = csv_data;
            state["error"] = "";
            std::cout << "Retrieved Data:\n" << csv_data << "\n" << std::endl;
        }
        duckdb_destroy_result(&result);

    } 
    catch (const std::exception& e){
        state["data"] = "";
        state["error"] = std::string("Error during lookup: ") + e.what();
        std::cerr << "[LookupSalesData] Exception: " << e.what() << std::endl;
    }
    return state;
}


static const std::string DATA_ANALYSIS_PROMPT = R"(You are a professional data analyst providing insights from query results.

## TASK
Answer the user's question based ONLY on the provided data.

## USER QUESTION
{prompt}

## AVAILABLE DATA
This data was retrieved using the SQL query: {sql_query}

Data:
{data}

## INSTRUCTIONS
1. Examine the data carefully to understand what information is available
2. Identify the key insights that directly answer the user's question
3. Provide a specific answer: aim for 2-3 sentences, but use up to 6 if needed to cover all key facts
4. Use actual numbers and facts from the data
5. Do NOT speculate or make assumptions beyond what the data shows
6. If the data doesn't fully answer the question, state what you can determine from the available data

## CHAIN OF THOUGHT REASONING
Before answering, think step by step:

**Step 1: Understanding the Question**
- What specific information is the user asking for?
- Is it asking for a single value, a comparison, a trend, or a summary?
- What would constitute a complete answer?

**Step 2: Examining the Data Structure**
- How many rows of data are available?
- What columns are present in the data?
- What is the range or distribution of values?
- Are there any patterns or anomalies visible?

**Step 3: Extracting Relevant Facts**
- Which specific values directly answer the question?
- Do I need to perform mental calculations (sum, average, count)?
- What are the exact numbers, dates, or categories relevant to the answer?
- Are there any context clues (time periods, units, categories)?

**Step 4: Verifying Completeness**
- Does the data fully answer the user's question?
- Is there missing information that prevents a complete answer?
- Should I mention any limitations or caveats?

**Step 5: Formulating the Answer**
- How can I state the facts concisely (2-3 sentences)?
- Am I using specific numbers from the data?
- Am I avoiding speculation or assumptions?
- Is my answer direct and clear?

## OUTPUT FORMAT
Provide a direct, concise answer in natural language (2-3 sentences). Focus only on facts from the data.
)";


/**
 * @brief Calcola somma, minimo, massimo e media per ogni colonna numerica del CSV e li accoda al testo.
 * 
 */
static std::string enrich_csv_with_stats(const std::string& csv_data){
    if (csv_data.empty()) 
        return csv_data;
    std::istringstream ss(csv_data);
    std::string line;
    if (!std::getline(ss, line)) 
        return csv_data;

    std::vector<std::string> headers;
    std::istringstream hss(line);
    std::string col;
    while(std::getline(hss, col, ',')) 
        headers.push_back(col); // salva i nomi delle colonne in headers

    size_t n_cols = headers.size();
    std::vector<std::vector<double>> num_vals(n_cols);
    std::vector<bool> is_numeric(n_cols, true);

    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        std::istringstream rss(line);
        for (size_t i = 0; i < n_cols; ++i) {
            std::string cell;
            if (!std::getline(rss, cell, ',')) 
                break;
            if (cell.empty()) 
                continue;
            try {
                size_t idx = 0;
                double v = std::stod(cell, &idx);
                if (idx == cell.size()) num_vals[i].push_back(v);
                else is_numeric[i] = false;
            } catch (...) {
                is_numeric[i] = false;
            }
        }
    }
    // Otteneniamo in num_vals i valori della tabella

    std::ostringstream out;
    out << csv_data << "\n--- Summary Statistics ---\n";
    for (size_t i = 0; i < n_cols; ++i) {
        const auto& v = num_vals[i];
        if (is_numeric[i] && !v.empty()) {
            double sum = std::accumulate(v.begin(), v.end(), 0.0);
            auto [mn_it, mx_it] = std::minmax_element(v.begin(), v.end());
            double mean = sum / v.size();

            out << std::fixed << std::setprecision(2)
                << headers[i] << " -> sum: " << sum 
                << ", min: " << *mn_it
                << ", max: " << *mx_it 
                << ", mean: " << mean << "\n";
        }
    }
    return out.str();
}

JsonObject AnalyzingData(JsonObject& state, std::shared_ptr<agents::Context> context){
    if(state.contains("error") && !state["error"].get<std::string>().empty()) 
        return state;

    std::string prompt = state.value("prompt", "");
    std::string sql_query = state.value("sql_query", "");
    std::string enriched_data = enrich_csv_with_stats(state.value("data", ""));

    std::string formatted_prompt = DATA_ANALYSIS_PROMPT;
    replace_all(formatted_prompt, "{prompt}", prompt);
    replace_all(formatted_prompt, "{sql_query}", sql_query);
    replace_all(formatted_prompt, "{data}", enriched_data);

    try{
        std::string formatted_cot_prompt = format_cot_prompt(formatted_prompt, state);
        auto llm = context->getLLM();
        auto response = llm->chat(formatted_cot_prompt);
        std::string analysis_text = response.content;

        std::cout << "Analysis:\n" << analysis_text << "\n" << std::endl;

        if(!state.contains("answer") || !state["answer"].is_array())
            state["answer"] = nlohmann::json::array();
        
        state["answer"].push_back(analysis_text);
    } 
    catch (const std::exception& e) {
        state["error"] = std::string("Error analyzing data: ") + e.what();
        std::cerr << "[AnalyzingData] Error: " << e.what() << std::endl;
    }
    return state;
}

static const std::string DECISION_PROMPT = R"(You are a workflow orchestrator managing a data analysis pipeline.

    ## AVAILABLE TOOLS
    - lookup_sales_data: Retrieves data from the database using SQL
    - analyzing_data: Analyzes retrieved data and provides insights
    - create_visualization: Generates chart code to visualize the data
    - end: Completes the workflow

    ## DECISION RULES (CRITICAL - Follow in order)
    1. Data prerequisite: Must run lookup_sales_data BEFORE analyzing_data or create_visualization
    2. No repetition: NEVER select a tool that has already been used
    3. Completion criteria: Select 'end' when:
    - 2 or more answers have been generated (analysis + visualization complete)
    - All relevant tools for the user's request have been executed

    ## DECISION FLOWCHART
    Start → Has data? No → lookup_sales_data
                ↓ Yes
            Already analyzed? No → analyzing_data
                ↓ Yes
            Need visualization? Yes → create_visualization
                ↓ No/Done
            end

    ## CURRENT STATE
    - User's request: {prompt}
    - Answers generated so far: {answer}
    - Visualization goal: {visualization_goal}
    - Last tool used: {tool_choice}

    ## CHAIN OF THOUGHT REASONING
    Before selecting the next tool, think step by step:

    **Step 1: Analyzing User Request**
    - What is the user asking for? (data lookup, analysis, visualization, or combination)
    - Does the request explicitly or implicitly require a chart/graph?
    - Is this a simple data retrieval or complex multi-step task?

    **Step 2: Checking Current Progress**
    - What tools have already been executed? (check Last tool used)
    - Do we have data available? (check if lookup_sales_data was run)
    - How many answers have been generated? (check Answers generated so far)
    - What stage of the workflow are we in?

    **Step 3: Identifying What's Missing**
    - If no data: Need lookup_sales_data first (Rule 1)
    - If data exists but no analysis: Need analyzing_data
    - If analysis exists but user wants visualization: Need create_visualization
    - If all required steps done: Need end

    **Step 4: Applying Decision Rules**
    - Rule 1 check: Do I have data before attempting analysis/visualization?
    - Rule 2 check: Am I about to repeat a tool already used?
    - Rule 3 check: Have I completed all necessary steps (2+ answers OR all relevant tools)?

    **Step 5: Making the Decision**
    - Based on steps 1-4, which tool should execute next?
    - Does this choice follow the DECISION FLOWCHART?
    - Is this the minimum necessary step to progress toward completion?

    ## EXAMPLES WITH REASONING

    Example 1 - Initial state:
    State: prompt="Show sales data", answer=[], tool_choice=None

    Reasoning:
    - Step 1: User wants sales data (implies lookup needed)
    - Step 2: No tools executed yet, no data, no answers
    - Step 3: Missing everything - start with data retrieval
    - Step 4: Rule 1 applies - need data first, Rule 2 N/A (nothing used), Rule 3 not met (0 answers)
    - Step 5: Must start with lookup_sales_data

    Decision: lookup_sales_data (need data first)

    Example 2 - After data lookup:
    State: prompt="Show sales data", answer=[], tool_choice="lookup_sales_data", data exists

    Reasoning:
    - Step 1: User wants sales data shown (implies analysis/presentation needed)
    - Step 2: lookup_sales_data executed, data available, but 0 answers generated
    - Step 3: Have data, missing analysis
    - Step 4: Rule 1 satisfied (have data), Rule 2 check (can't repeat lookup), Rule 3 not met (0 answers)
    - Step 5: Next logical step is analyzing_data

    Decision: analyzing_data (have data, now analyze)

    Example 3 - After analysis and visualization:
    State: prompt="Show sales trends", answer=["Analysis text", "Chart code"], tool_choice="create_visualization"

    Reasoning:
    - Step 1: User wanted trends (implies analysis + visualization)
    - Step 2: All tools executed, 2 answers generated (analysis + chart code)
    - Step 3: Nothing missing - workflow complete
    - Step 4: Rule 1 satisfied, Rule 2 satisfied, Rule 3 MET (2+ answers generated)
    - Step 5: Should end the workflow

    Decision: end (2+ answers generated, workflow complete)

    Example 4 - After analysis only (no viz needed):
    State: prompt="What were total sales?", answer=["Total sales were $X"], tool_choice="analyzing_data"

    Reasoning:
    - Step 1: User wanted a simple factual answer (no visualization implied)
    - Step 2: lookup and analysis executed, 1 answer generated
    - Step 3: Question fully answered with analysis alone
    - Step 4: Rule 1 satisfied, Rule 2 satisfied, Rule 3 check (all RELEVANT tools done)
    - Step 5: No visualization needed for this query - can end

    Decision: end (all relevant tools executed, question answered)

    Example 5 - After lookup only (viz needed):
    State: prompt="Show me a chart of monthly sales", answer=[], tool_choice="lookup_sales_data", data exists

    Reasoning:
    - Step 1: User explicitly wants a chart (visualization required)
    - Step 2: Only lookup executed, data available, 0 answers
    - Step 3: Missing both analysis AND visualization
    - Step 4: Rule 1 satisfied (have data), Rule 2 satisfied (not repeating), Rule 3 not met (0 answers)
    - Step 5: Should analyze first, then visualize (follow flowchart)

    Decision: analyzing_data (analyze before visualizing)

    ## YOUR TASK
    Based on the chain of thought reasoning above and the current state, select the next tool to execute.

    ## OUTPUT FORMAT
    Respond with ONLY the tool name: lookup_sales_data, analyzing_data, create_visualization, or end
    No explanations. Just the tool name.)";

JsonObject DecideTool(JsonObject& state, std::shared_ptr<agents::Context> context){
    if(state.contains("error") && !state["error"].get<std::string>().empty()) 
        return state;

    std::string prompt = state.value("prompt", "");
    std::string answer = state.contains("answer") ? state["answer"].dump() : "[]";
    std::string visualization_goal = state.value("visualization_goal", "");
    std::string last_tool = state.value("tool_choice", "None");
    bool has_data = state.contains("data") && !state["data"].get<std::string>().empty();

    std::string formatted_prompt = DECISION_PROMPT;
    replace_all(formatted_prompt, "{prompt}", prompt);
    replace_all(formatted_prompt, "{answer}", answer);
    replace_all(formatted_prompt, "{visualization_goal}", visualization_goal);
    replace_all(formatted_prompt, "{tool_choice}", last_tool);

    try{
        std::string formatted_cot_prompt = format_cot_prompt(formatted_prompt, state);
        auto llm = context->getLLM();
        auto response = llm->chat(formatted_cot_prompt);
        std::string raw_response = response.content;

        std::transform(raw_response.begin(), raw_response.end(), raw_response.begin(), ::tolower);

        const std::vector<std::string> valid_tools = {
            "lookup_sales_data",
            "create_visualization",
            "analyzing_data",
            "end"
        };
        
        std::string matched_tool = "";
        //Se la risposta dell'LLM contiene il nome del tool, lo restituisce
        for (const auto& tool : valid_tools){
            if (raw_response.find(tool) != std::string::npos){
                matched_tool = tool;
                break;
            }
        }

        // Guard rails
        if ((matched_tool == "analyzing_data" || matched_tool == "create_visualization") && !has_data)
            matched_tool = "lookup_sales_data";
        if (matched_tool == "lookup_sales_data" && last_tool == "lookup_sales_data" && !has_data)
            matched_tool = "end";
        else if (state.contains("answer") && state["answer"].is_array() && state["answer"].size() > 1)
            matched_tool = "end";

        state["tool_choice"] = matched_tool;
    }
    catch (const std::exception& e) {
        state["error"] = std::string("Error choosing tool: ") + e.what();
        std::cerr << "[DecideTool] Error: " << e.what() << std::endl;
    }
return state;
}


static const std::string CHART_CONFIGURATION_PROMPT = R"(You are a data visualization expert designing chart configurations.

## TASK
Create a JSON configuration object for visualizing the provided data.

## VISUALIZATION GOAL
{visualization_goal}

## DATA TO VISUALIZE
{data}

## CHART TYPE SELECTION GUIDE
Choose the appropriate chart type based on the data and goal:
- bar: Comparing discrete categories or groups (e.g., sales by product, revenue by region)
- line: Showing trends over time or continuous progression (e.g., monthly sales, daily visitors)
- scatter: Showing correlations or relationships between two variables (e.g., price vs. demand)
- area: Showing volume or cumulative values over time (e.g., cumulative revenue, market share)

## REQUIRED JSON KEYS
- chart_type: One of [bar, line, area, scatter]
- x_axis: Column name for X-axis (string)
- y_axis: Column name for Y-axis (string) — use this for SINGLE-series charts and for long-format grouped bar charts (used together with group_by)
- y_axes: List of column names for Y-axis (list of strings) — use this INSTEAD of y_axis when the DataFrame already has one column per series (wide format). Do NOT include both y_axis and y_axes.
- group_by: (OPTIONAL) Column name whose distinct values define the bar series in a long-format grouped bar chart. Use this together with y_axis when the data has a discriminator column (e.g., 'year', 'quarter') instead of separate columns per series. Do NOT use together with y_axes.
- title: Descriptive chart title (string)

## WHEN TO USE y_axes vs y_axis vs group_by
- Use y_axis (single string) when showing ONE metric: revenue, count, score
- Use y_axes (list) when the DataFrame already has one column per series — i.e., the series values are in separate columns (e.g., Avg_Revenue_Promo, Avg_Revenue_Non_Promo)
- Use y_axis + group_by when data is in LONG FORMAT with a discriminator column: the same metric column (y_axis) appears for multiple groups identified by another column (group_by). Example: data has columns (region, year, avg_monthly_revenue) and you want separate bars for each year → x_axis=region, y_axis=avg_monthly_revenue, group_by=year

## CHAIN OF THOUGHT REASONING
Before creating the configuration, think step by step:

**Step 1: Understanding the Visualization Goal**
- What story does the user want to tell with this chart?
- Is the goal to compare, show trends, find correlations, or display distributions?
- Are there any keywords that hint at chart type? (e.g., "over time" → line, "compare" → bar)

**Step 2: Analyzing the Data Structure**
- What columns are available in the data?
- Which columns contain categorical data (text, discrete values)?
- Which columns contain numerical data (integers, floats)?
- Are there any date/time columns?
- How many rows of data are there (affects visualization approach)?

**Step 3: Selecting Chart Type**
- For comparisons between categories → bar chart
- For trends over time or continuous sequences → line chart
- For showing relationships between two numeric variables → scatter plot
- For cumulative values over time → area chart
- Does the data structure support this chart type?

**Step 4: Mapping Axes**
- What should go on the X-axis? (independent variable, categories, or time)
- What should go on the Y-axis? (dependent variable, values, metrics)
- Do the chosen columns make logical sense for these axes?
- For time series: dates on X-axis, metrics on Y-axis
- For comparisons: categories on X-axis, values on Y-axis

**Step 5: Creating the Title**
- What concise phrase describes what the chart shows?
- Include the key variables or metrics being displayed
- Format: "[Metric] by/over/vs [Variable]" or similar clear structure
- Examples: "Revenue Over Time", "Sales by Product", "Price vs Demand"

Now, based on this reasoning, create the JSON configuration.

## EXAMPLES WITH REASONING

Example 1 - Time series data:
Data columns: Date, Revenue
Goal: "Show revenue trends over time"

Reasoning:
- Step 1: Goal mentions "trends over time" → time series visualization
- Step 2: Columns: Date (temporal), Revenue (numeric), likely multiple rows
- Step 3: "Over time" + "trends" → line chart is appropriate
- Step 4: X-axis = Date (time progression), Y-axis = Revenue (metric being tracked)
- Step 5: Title: "Revenue Trends Over Time" (clear, includes both variables)

Output: {{"chart_type": "line", "x_axis": "Date", "y_axis": "Revenue", "title": "Revenue Trends Over Time"}}

Example 2 - Categorical comparison:
Data columns: Product_Name, Units_Sold
Goal: "Compare products by units sold"

Reasoning:
- Step 1: Goal says "compare" → comparison visualization
- Step 2: Columns: Product_Name (categorical), Units_Sold (numeric)
- Step 3: Comparing discrete categories → bar chart is appropriate
- Step 4: X-axis = Product_Name (categories), Y-axis = Units_Sold (values to compare)
- Step 5: Title: "Units Sold by Product" (shows what's being compared)

Output: {{"chart_type": "bar", "x_axis": "Product_Name", "y_axis": "Units_Sold", "title": "Units Sold by Product"}}

Example 3 - Correlation analysis:
Data columns: Price, Demand, Product_ID
Goal: "Analyze the relationship between price and demand"

Reasoning:
- Step 1: Goal mentions "relationship between" → correlation visualization
- Step 2: Columns: Price (numeric), Demand (numeric), Product_ID (identifier)
- Step 3: Two numeric variables, looking for correlation → scatter plot
- Step 4: X-axis = Price (independent variable), Y-axis = Demand (dependent variable)
- Step 5: Title: "Price vs Demand Analysis" (shows both variables being correlated)

Output: {{"chart_type": "scatter", "x_axis": "Price", "y_axis": "Demand", "title": "Price vs Demand Analysis"}}

Example 4 - Cumulative values:
Data columns: Month, Cumulative_Sales
Goal: "Show cumulative sales growth throughout the year"

Reasoning:
- Step 1: Goal mentions "cumulative" and "growth" → volume over time
- Step 2: Columns: Month (temporal), Cumulative_Sales (numeric, accumulating)
- Step 3: Cumulative values over time → area chart emphasizes volume
- Step 4: X-axis = Month (time), Y-axis = Cumulative_Sales (accumulated metric)
- Step 5: Title: "Cumulative Sales Growth" (describes the accumulation)

Output: {{"chart_type": "area", "x_axis": "Month", "y_axis": "Cumulative_Sales", "title": "Cumulative Sales Growth"}}

Example 5 - Regional comparison:
Data columns: Region, Average_Revenue, Store_Count
Goal: "Compare average revenue across different regions"

Reasoning:
- Step 1: Goal says "compare...across" → categorical comparison
- Step 2: Columns: Region (categorical), Average_Revenue (numeric), Store_Count (numeric)
- Step 3: Comparing categories → bar chart
- Step 4: X-axis = Region (categories), Y-axis = Average_Revenue (metric from goal)
- Step 5: Title: "Average Revenue by Region" (clear comparison statement)

Output: {{"chart_type": "bar", "x_axis": "Region", "y_axis": "Average_Revenue", "title": "Average Revenue by Region"}}

Example 6 - Multi-series comparison (grouped bar):
Data columns: Product_Class, Avg_Revenue_Promo, Avg_Revenue_Non_Promo, Abs_Difference
Goal: "Compare average revenue per unit during promotions vs non-promotions for each product class"

Reasoning:
- Step 1: Goal says "compare...vs" → TWO metrics side by side, not one
- Step 2: Columns: Product_Class (categorical), Avg_Revenue_Promo and Avg_Revenue_Non_Promo (both numeric, both needed)
- Step 3: Comparing two numeric series across categories → grouped bar chart
- Step 4: X-axis = Product_Class (categories), Y-axes = [Avg_Revenue_Promo, Avg_Revenue_Non_Promo] (both series)
- Step 5: Title clearly names both series being compared

Output: {{"chart_type": "bar", "x_axis": "Product_Class", "y_axes": ["Avg_Revenue_Promo", "Avg_Revenue_Non_Promo"], "title": "Average Revenue per Unit: Promo vs Non-Promo by Product Class"}}

Example 7 - Long-format grouped bar chart (data has a discriminator column, use group_by):
Data columns: region, year, avg_monthly_revenue (8 rows: 4 regions × 2 years, long format)
Goal: "Compare average monthly revenue by region for 2022 vs 2023"

Reasoning:
- Step 1: "Compare...for 2022 vs 2023" → two bar series, one per year
- Step 2: Columns: region (categorical x-axis), year (discriminator: values 2022 or 2023), avg_monthly_revenue (metric). Data is LONG FORMAT — one row per (region, year). There are NO separate columns avg_monthly_revenue_2022 / avg_monthly_revenue_2023.
- Step 3: Two series over categories → grouped bar chart
- Step 4: x_axis = region, y_axis = avg_monthly_revenue (the metric), group_by = year (to split into two bar series). Do NOT use y_axes here — that would require wide-format columns which don't exist.
- Step 5: Title names both variables being compared

Output: {{"chart_type": "bar", "x_axis": "region", "y_axis": "avg_monthly_revenue", "group_by": "year", "title": "Avg Monthly Revenue by Region: 2022 vs 2023"}}


## OUTPUT FORMAT
Return ONLY a valid JSON object. No markdown. No code fences. No backticks. No explanations. Just the JSON.)";

/**
 * @brief Estrae il primo oggetto JSON valido dalla risposta grezza dell'LLM.
 */
static JsonObject parse_chart_config(const std::string& raw_output){
    // Trova l'inizio '{' e la fine '}' del JSON ignorando eventuale testo o markdown attorno
    size_t start = raw_output.find('{');
    size_t end = raw_output.rfind('}');

    if (start == std::string::npos || end == std::string::npos || end <= start) {
        std::cerr << "[parse_chart_config] Nessun blocco JSON trovato nell'output.\n";
        return JsonObject{{"chart_type", "bar"}}; 
    }

    std::string json_str = raw_output.substr(start, end - start + 1);

    try {
        return JsonObject::parse(json_str);
    } catch (const std::exception& e) {
        std::cerr << "[parse_chart_config] Errore di parsing JSON: " << e.what() << "\n";
        return JsonObject{{"chart_type", "bar"}};
    }
}


static const std::string CREATE_CHART_PROMPT = R"(You are a Python data visualization developer creating matplotlib charts.

## TASK
Generate Python code to create a chart based on the provided configuration.

## AVAILABLE IN SCOPE
- data_df: pandas DataFrame with the data (already loaded, do NOT create it)
- config: Dictionary with chart configuration (already defined, do NOT create it)
- pd: pandas module (already imported)
- plt: matplotlib.pyplot module (already imported)

## CHART CONFIGURATION
{config}

## REQUIREMENTS
Your code must:
1. Import matplotlib.pyplot as plt
2. Import pandas as pd (if needed for data manipulation)
3. Check whether config has 'y_axes' (list), 'group_by' (string), or 'y_axis' (string) and handle accordingly:
   - If config has 'y_axes': data is in WIDE FORMAT — produce a GROUPED BAR chart, one bar series per column in y_axes (data_df[col] for each col)
   - If config has 'group_by': data is in LONG FORMAT — produce a GROUPED BAR chart by filtering data_df by each unique value of config['group_by'], using config['y_axis'] as the metric column. Use sorted unique values of data_df[config['group_by']] as series labels.
   - If config has 'y_axis' only: access data with data_df[config['y_axis']] as usual (single series)
4. Create the appropriate chart type using config['chart_type']
5. Set the chart title using config['title']
6. Add axis labels, and a legend when multiple series are present
7. Call plt.tight_layout() before plt.show()
8. Call plt.show() at the end

## CHART TYPE IMPLEMENTATIONS

### Bar Chart (chart_type='bar'):
- Use plt.bar(x_data, y_data) for vertical bars
- Good for categorical comparisons

### Line Chart (chart_type='line'):
- Use plt.plot(x_data, y_data) for lines
- Good for time series and trends

### Scatter Plot (chart_type='scatter'):
- Use plt.scatter(x_data, y_data) for points
- Good for correlations

### Area Chart (chart_type='area'):
- Use plt.fill_between(x_data, y_data) for filled areas
- Good for cumulative values

## CRITICAL: X-AXIS LABEL OVERLAP PREVENTION
**ALWAYS check and prevent x-axis label overlapping:**
- For categorical data with many categories (>10): rotate labels 45° or 90° AND use ha='right'
- For long text labels: ALWAYS rotate even if few labels
- For dates: rotate 45° with ha='right'
- If labels are still crowded after rotation: consider reducing font size with fontsize=8
- Alternative strategies:
  * Use plt.xticks(rotation=45, ha='right', fontsize=9) for crowded labels
  * Use plt.xticks(rotation=90) for very long labels
  * Consider abbreviating labels if possible
  * Increase figure width with plt.figure(figsize=(12, 6)) for many data points

## CHAIN OF THOUGHT REASONING
Before writing the code, think step by step:

**Step 1: Understanding the Configuration**
- What chart type is requested? (bar, line, scatter, area)
- Does config have 'y_axes' (list → multi-series grouped) or 'y_axis' (string → single series)?
- What is the title for the chart?
- Are there any special characteristics suggested by the column names?

**Step 2: Planning Data Extraction**
- How do I access the x-axis data? (data_df[config['x_axis']])
- Single series: data_df[config['y_axis']]
- Wide-format multi-series (y_axes): iterate over config['y_axes'], plot data_df[col] for each, using numpy offsets
- Long-format multi-series (group_by): get sorted unique values of data_df[config['group_by']], filter data_df by each value, extract config['y_axis'] values, plot using numpy offsets
- Do I need to handle special data types (dates, categories)?
- Should I sort or transform the data before plotting?

**Step 3: Selecting Matplotlib Function**
- For bar: plt.bar(x_data, y_data)
- For line: plt.plot(x_data, y_data)
- For scatter: plt.scatter(x_data, y_data)
- For area: plt.fill_between(x_data, y_data)
- What additional parameters improve readability? (marker, alpha, etc.)

**Step 4: Adding Chart Enhancements**
- Axis labels: plt.xlabel() and plt.ylabel() using config keys
- Title: plt.title() using config['title']
- **CRITICAL - Check X-axis label overlap potential:**
  * How many data points are there?
  * Are x-axis labels text (categorical) or dates?
  * Are the labels likely long (product names, location names)?
  * Decision: Apply rotation and alignment to prevent overlap
- For time series or scatter: add grid with plt.grid(True, alpha=0.3)
- Any other styling needed?

**Step 5: Finalizing and Rendering**
- Call plt.tight_layout() to prevent label cutoff (CRITICAL after rotation)
- Call plt.show() to display the chart
- Verify all requirements are met (imports, data access, chart type, labels, title)
- Double-check no syntax errors or missing steps

Now, based on this reasoning, generate the Python code.

## EXAMPLES WITH REASONING

Example 1 - Bar chart with categorical data:
config = {{"chart_type": "bar", "x_axis": "Product", "y_axis": "Sales", "title": "Sales by Product"}}

Reasoning:
- Step 1: Bar chart, x=Product (categorical), y=Sales (numeric), title provided
- Step 2: Extract data_df['Product'] and data_df['Sales'], no special handling needed
- Step 3: Use plt.bar(x_data, y_data) for vertical bars
- Step 4: Add labels, **Product names likely long → MUST rotate to prevent overlap**, apply rotation=45, ha='right'
- Step 5: tight_layout() (critical for rotated labels) then show()

Code:
import matplotlib.pyplot as plt
import pandas as pd

x_data = data_df[config['x_axis']]
y_data = data_df[config['y_axis']]

plt.figure(figsize=(10, 6))
plt.bar(x_data, y_data)
plt.xlabel(config['x_axis'])
plt.ylabel(config['y_axis'])
plt.title(config['title'])
plt.xticks(rotation=45, ha='right')
plt.tight_layout()
plt.show()

Example 2 - Line chart with dates:
config = {{"chart_type": "line", "x_axis": "Date", "y_axis": "Revenue", "title": "Revenue Over Time"}}

Reasoning:
- Step 1: Line chart, x=Date (temporal), y=Revenue (numeric), time series visualization
- Step 2: Extract data, x-axis is dates
- Step 3: Use plt.plot(x_data, y_data) with marker='o' to show data points
- Step 4: Add labels, **dates on x-axis → rotate to prevent overlap**, add grid for trends
- Step 5: tight_layout() then show()

Code:
import matplotlib.pyplot as plt
import pandas as pd

x_data = data_df[config['x_axis']]
y_data = data_df[config['y_axis']]

plt.figure(figsize=(12, 6))
plt.plot(x_data, y_data, marker='o')
plt.xlabel(config['x_axis'])
plt.ylabel(config['y_axis'])
plt.title(config['title'])
plt.xticks(rotation=45, ha='right')
plt.grid(True, alpha=0.3)
plt.tight_layout()
plt.show()

Example 3 - Scatter plot (no rotation needed):
config = {{"chart_type": "scatter", "x_axis": "Price", "y_axis": "Demand", "title": "Price vs Demand"}}

Reasoning:
- Step 1: Scatter plot, x=Price (numeric), y=Demand (numeric), correlation analysis
- Step 2: Extract both numeric columns, no special handling
- Step 3: Use plt.scatter(x_data, y_data) with alpha=0.6 for overlapping points
- Step 4: Add labels, **numeric x-axis → no rotation needed**, add grid for patterns
- Step 5: tight_layout() then show()

Code:
import matplotlib.pyplot as plt
import pandas as pd

x_data = data_df[config['x_axis']]
y_data = data_df[config['y_axis']]

plt.figure(figsize=(10, 6))
plt.scatter(x_data, y_data, alpha=0.6)
plt.xlabel(config['x_axis'])
plt.ylabel(config['y_axis'])
plt.title(config['title'])
plt.grid(True, alpha=0.3)
plt.tight_layout()
plt.show()

Example 4 - Area chart with months:
config = {{"chart_type": "area", "x_axis": "Month", "y_axis": "Cumulative_Sales", "title": "Cumulative Sales Growth"}}

Reasoning:
- Step 1: Area chart, x=Month (temporal/sequential), y=Cumulative_Sales (numeric), shows volume
- Step 2: Extract data, ensure x is in proper order
- Step 3: Use plt.fill_between(x_data, y_data) to create filled area
- Step 4: Add labels, **month names (text) → rotate to prevent overlap**, grid for progression
- Step 5: tight_layout() then show()

Code:
import matplotlib.pyplot as plt
import pandas as pd

x_data = data_df[config['x_axis']]
y_data = data_df[config['y_axis']]

plt.figure(figsize=(12, 6))
plt.fill_between(x_data, y_data, alpha=0.4)
plt.plot(x_data, y_data)
plt.xlabel(config['x_axis'])
plt.ylabel(config['y_axis'])
plt.title(config['title'])
plt.xticks(rotation=45, ha='right')
plt.grid(True, alpha=0.3)
plt.tight_layout()
plt.show()

Example 5 - Bar chart with many categories:
config = {{"chart_type": "bar", "x_axis": "Store_Location", "y_axis": "Revenue", "title": "Revenue by Store Location"}}

Reasoning:
- Step 1: Bar chart, x=Store_Location (categorical, likely many stores), y=Revenue
- Step 2: Extract data, many categories expected
- Step 3: Use plt.bar(x_data, y_data)
- Step 4: **Many stores + location names (long text) → CRITICAL overlap risk**, rotate 45°, increase figure width, reduce font size
- Step 5: tight_layout() essential for rotated labels

Code:
import matplotlib.pyplot as plt
import pandas as pd

x_data = data_df[config['x_axis']]
y_data = data_df[config['y_axis']]

plt.figure(figsize=(14, 6))
plt.bar(x_data, y_data)
plt.xlabel(config['x_axis'])
plt.ylabel(config['y_axis'])
plt.title(config['title'])
plt.xticks(rotation=45, ha='right', fontsize=9)
plt.tight_layout()
plt.show()


Example 6 - Grouped bar chart (multi-series, config has 'y_axes'):
config = {{"chart_type": "bar", "x_axis": "Product_Class", "y_axes": ["Avg_Revenue_Promo", "Avg_Revenue_Non_Promo"], "title": "Promo vs Non-Promo Revenue by Product Class"}}

Reasoning:
- Step 1: Bar chart, config has 'y_axes' (list of 2) → grouped bar, multi-series
- Step 2: x = data_df['Product_Class'], iterate y_axes for each series; sort by absolute difference if available
- Step 3: Use numpy arange for x positions, offset each group by bar_width
- Step 4: Add legend for series, rotate x labels, add grid
- Step 5: tight_layout() then show()

Code:
import matplotlib.pyplot as plt
import pandas as pd
import numpy as np

x_labels = data_df[config['x_axis']].astype(str)
y_axes = config['y_axes']
n_series = len(y_axes)
bar_width = 0.8 / n_series
x = np.arange(len(x_labels))

plt.figure(figsize=(12, 6))
for i, col in enumerate(y_axes):
    offset = (i - n_series / 2 + 0.5) * bar_width
    plt.bar(x + offset, data_df[col], width=bar_width, label=col)

plt.xlabel(config['x_axis'])
plt.ylabel('Value')
plt.title(config['title'])
plt.xticks(x, x_labels, rotation=45, ha='right')
plt.legend()
plt.grid(True, axis='y', alpha=0.3)
plt.tight_layout()
plt.show()


Example 7 - Grouped bar from long-format data (config has 'group_by'):
config = {{"chart_type": "bar", "x_axis": "region", "y_axis": "avg_monthly_revenue", "group_by": "year", "title": "Avg Monthly Revenue by Region: 2022 vs 2023"}}

Reasoning:
- Step 1: Bar chart, config has 'group_by' = 'year' → data is in LONG FORMAT, split into series by year value
- Step 2: Get sorted unique year values (e.g., [2022, 2023]). For each year, filter data_df and extract avg_monthly_revenue indexed by region.
- Step 3: Use numpy arange for x positions, offset bars for each year group
- Step 4: Add legend with year values as labels, rotate x labels for region names
- Step 5: tight_layout() then show()

Code:
import matplotlib.pyplot as plt
import pandas as pd
import numpy as np

group_col = config['group_by']
x_col = config['x_axis']
y_col = config['y_axis']

groups = sorted(data_df[group_col].unique())
x_labels = sorted(data_df[x_col].unique())
x = np.arange(len(x_labels))
n_series = len(groups)
bar_width = 0.8 / n_series

plt.figure(figsize=(12, 6))
for i, group_val in enumerate(groups):
    df_group = data_df[data_df[group_col] == group_val].set_index(x_col)
    y_vals = [df_group.loc[xl, y_col] if xl in df_group.index else 0 for xl in x_labels]
    offset = (i - n_series / 2 + 0.5) * bar_width
    plt.bar(x + offset, y_vals, width=bar_width, label=str(group_val))

plt.xlabel(x_col)
plt.ylabel(y_col)
plt.title(config['title'])
plt.xticks(x, x_labels, rotation=45, ha='right')
plt.legend()
plt.grid(True, axis='y', alpha=0.3)
plt.tight_layout()
plt.show()


## OUTPUT FORMAT
Return ONLY the Python code. No markdown formatting. No code fences. No explanations. Just the executable Python code.)";

/**
 * @brief Valida ed esegue il codice Matplotlib generato salvando il grafico in `Output/chart.png`.
 * @param chart_code Codice Python generato dall'LLM.
 * @param chart_config Oggetto JSON di configurazione del grafico.
 * @param csv_file_path Percorso al file CSV con i dati estratti.
 * @param[out] out_error Stringa popolata con il traceback Python in caso di fallimento.
 * @return true se lo script viene eseguito senza errori, false altrimenti.
 */
static bool execute_matplotlib_code(const std::string& chart_code, const JsonObject& chart_config, const std::string& csv_file_path, std::string& out_error) {
    std::ostringstream script;
    script << "import matplotlib\n";
    script << "matplotlib.use('Agg')\n";
    script << "import matplotlib.pyplot as plt\n";
    script << "import pandas as pd\n\n";
    
    script << "data_df = pd.read_csv('" << csv_file_path << "')\n";
    script << "config = " << chart_config.dump() << "\n\n";
    
    script << "# --- Generated Code ---\n";
    script << chart_code << "\n";
    script << "# Salva il grafico generato\n";
    script << "plt.savefig('Output/chart.png')\n";
    script << "plt.close('all')\n";

    std::string script_path = "Output/temp_plot.py";
    std::ofstream py_file(script_path);
    if (!py_file.is_open()) {
        out_error = "Impossibile creare il file temporaneo Python";
        return false;
    }
    py_file << script.str();
    py_file.close();

    std::string cmd = "python3 " + script_path + " 2>&1";
    std::array<char, 256> buffer;
    std::string process_output;

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        out_error = "Fallita apertura pipe per l'interprete Python.";
        return false;
    }

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        process_output += buffer.data();
    }

    int return_code = pclose(pipe);

    if (return_code != 0) {
        out_error = process_output;
        return false;
    }

    return true;
}

JsonObject CreateVisualization(JsonObject& state, std::shared_ptr<agents::Context> context){

    if(state.contains("error") && !state["error"].get<std::string>().empty()) 
        return state;

    std::string visualization_goal = state.value("visualization_goal", "");
    if (visualization_goal.empty())
        visualization_goal = state.value("prompt", "");
    std::string data = state.value("data", "");

    std::string formatted_configuration_prompt = CHART_CONFIGURATION_PROMPT;
    replace_all(formatted_configuration_prompt, "{data}", data);
    replace_all(formatted_configuration_prompt, "{visualization_goal}", visualization_goal);

    JsonObject config;

    try{
        std::string formatted_cot_configuration_prompt = format_cot_prompt(formatted_configuration_prompt, state);
        auto llm = context->getLLM();
        auto response = llm->chat(formatted_cot_configuration_prompt);
        std::string raw_response = response.content;

        config = parse_chart_config(raw_response);
        state["chart_config"] = config;

        std::string formatted_chart_prompt = CREATE_CHART_PROMPT;
        replace_all(formatted_chart_prompt, "{config}", config.dump());

        std::string formatted_cot_chart_prompt = format_cot_prompt(formatted_chart_prompt, state);

        auto code_response = llm->chat(formatted_cot_chart_prompt);
        std::string chart_code = code_response.content;

        // Pulisci eventuali code fences markdown e plt.show()
        replace_all(chart_code, "```python", "");
        replace_all(chart_code, "```", "");
        replace_all(chart_code, "plt.show()", "");

        std::string out_error = "";
        std::string csv_file_path = "Output/result.csv"; 

        if (!execute_matplotlib_code(chart_code, config, csv_file_path, out_error)) {
            state["error"] = "Visualization code error: " + out_error;
            std::cerr << "[create_visualization] Validation Error:\n" << out_error << std::endl;
        }
        else 
            state["error"] = "";


        if (!state.contains("answer") || !state["answer"].is_array())
            state["answer"] = nlohmann::json::array();
        state["answer"].push_back(chart_code);

    }
    catch (const std::exception& e) {
        state["error"] = std::string("Error creating the chart: ") + e.what();
        std::cerr << "[CreateVisualozation] Error: " << e.what() << std::endl;
    }
    return state;
}


}