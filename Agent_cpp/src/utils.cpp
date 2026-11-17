#include "utils.h"
#include <iostream>
#include <sstream>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <vector>
#include <numeric>
#include <cmath>
#include <iomanip>
#include <difflib.h>

namespace agent_cpp {

void replace_all(std::string& str, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
}

std::string query_result_to_csv(duckdb_result& result) {
    std::stringstream ss;
    idx_t col_count = duckdb_column_count(&result);
    idx_t row_count = duckdb_row_count(&result);

    for (idx_t col = 0; col < col_count; ++col) {
        const char* name = duckdb_column_name(&result, col);
        ss << (name ? name : "") << (col < col_count - 1 ? "," : "");
    }
    ss << "\n";

    for (idx_t row = 0; row < row_count; ++row) {
        for (idx_t col = 0; col < col_count; ++col) {
            char* val = duckdb_value_varchar(&result, col, row);
            ss << (val ? val : "") << (col < col_count - 1 ? "," : "");
            if (val) duckdb_free(val);
        }
        ss << "\n";
    }

    std::string csv_content = ss.str();

    namespace fs = std::filesystem;
    fs::create_directories("Output");

    std::ofstream out_file("Output/result.csv");
    if (out_file.is_open()) {
        out_file << csv_content;
        out_file.close();
    } else {
        std::cerr << "[LookupSalesData] Errore apertura file in Output/result.csv\n";
    }

    return csv_content;
}

bool init_database(duckdb_connection con, const DatabaseSchema& schema) {
    for (const auto& table : schema.tables) {
        std::string query = "CREATE TABLE " + table.name + " AS SELECT * FROM read_parquet('" + table.file_path + "');";
        duckdb_result res;
        if (duckdb_query(con, query.c_str(), &res) == DuckDBError) {
            std::cerr << "[LookupSalesData] Errore caricamento tabella " << table.name 
                      << ": " << (duckdb_result_error(&res) ? duckdb_result_error(&res) : "unknown error") << std::endl;
            duckdb_destroy_result(&res);
            return false;
        }
        duckdb_destroy_result(&res);
    }
    return true;
}

double sequence_similarity(const std::string& a, const std::string& b) {
    difflib::SequenceMatcher<std::string> sm(a, b);
    return sm.ratio();
}

static std::string trim_str(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::vector<std::string> parse_csv_line(const std::string& line) {
    std::vector<std::string> cells;
    std::string cell;
    bool in_quotes = false;

    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') {
            in_quotes = !in_quotes;
        } else if (c == ',' && !in_quotes) {
            cells.push_back(trim_str(cell));
            cell.clear();
        } else {
            cell += c;
        }
    }
    cells.push_back(trim_str(cell));
    return cells;
}

CsvTable parse_csv_string(const std::string& csv_str) {
    CsvTable table;
    std::stringstream ss(csv_str);
    std::string line;
    bool is_header = true;

    while (std::getline(ss, line)) {
        std::string trimmed = trim_str(line);
        if (trimmed.empty()) continue;

        auto cells = parse_csv_line(trimmed);
        if (is_header) {
            for (auto& h : cells) {
                // Lowercase column headers for comparison
                std::string col = h;
                std::transform(col.begin(), col.end(), col.begin(), ::tolower);
                table.columns.push_back(col);
            }
            is_header = false;
        } else {
            table.rows.push_back(cells);
        }
    }
    return table;
}

static bool row_cells_match(
    const std::vector<std::string>& r1,
    const std::vector<std::string>& r2,
    const std::vector<std::pair<size_t, size_t>>& col_indices,
    double atol
) {
    for (const auto& [i1, i2] : col_indices) {
        if (i1 >= r1.size() || i2 >= r2.size()) return false;
        const std::string& a = r1[i1];
        const std::string& b = r2[i2];

        try {
            size_t idx1 = 0, idx2 = 0;
            double v1 = std::stod(a, &idx1);
            double v2 = std::stod(b, &idx2);
            if (idx1 == a.size() && idx2 == b.size()) {
                if (std::abs(v1 - v2) <= atol) continue;
                return false;
            }
        } catch (...) {}

        std::string sa = a;
        std::string sb = b;
        replace_all(sa, " 00:00:00", "");
        replace_all(sb, " 00:00:00", "");

        if (sa == sb) continue;

        // Date prefix equivalence: YYYY-MM vs YYYY-MM-01
        if (sa.size() >= 7 && sb.size() >= 7 && sa.substr(0, 7) == sb.substr(0, 7)) {
            if ((sa.size() == 7 && sb.size() == 10 && sb.rfind("-01") == 7) ||
                (sb.size() == 7 && sa.size() == 10 && sa.rfind("-01") == 7)) {
                continue;
            }
        }

        return false;
    }
    return true;
}

double compare_csv_tables_iou(const CsvTable& t1, const CsvTable& t2, double atol) {
    if (t1.rows.empty() || t2.rows.empty()) return 0.0;

    std::vector<std::pair<size_t, size_t>> col_indices;

    if (t1.columns == t2.columns) {
        for (size_t i = 0; i < t1.columns.size(); ++i) {
            col_indices.emplace_back(i, i);
        }
    } else {
        for (size_t i = 0; i < t1.columns.size(); ++i) {
            for (size_t j = 0; j < t2.columns.size(); ++j) {
                if (t1.columns[i] == t2.columns[j]) {
                    col_indices.emplace_back(i, j);
                    break;
                }
            }
        }
        if (col_indices.empty()) return 0.0;
    }

    std::vector<bool> used(t2.rows.size(), false);
    size_t matched = 0;

    for (const auto& r1 : t1.rows) {
        for (size_t j = 0; j < t2.rows.size(); ++j) {
            if (!used[j] && row_cells_match(r1, t2.rows[j], col_indices, atol)) {
                used[j] = true;
                matched++;
                break;
            }
        }
    }

    size_t total = t1.rows.size() + t2.rows.size() - matched;
    return (total > 0) ? (static_cast<double>(matched) / static_cast<double>(total)) : 0.0;
}

std::vector<double> evaluate_csv_consensus(const std::vector<std::string>& csv_list) {
    size_t n = csv_list.size();
    if (n <= 1) return std::vector<double>(n, 1.0);

    std::vector<CsvTable> tables;
    tables.reserve(n);
    for (const auto& csv : csv_list) {
        tables.push_back(parse_csv_string(csv));
    }

    std::vector<double> scores(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        if (tables[i].rows.empty()) continue;
        double total = 0.0;
        int count = 0;
        for (size_t j = 0; j < n; ++j) {
            if (i == j || tables[j].rows.empty()) continue;
            total += compare_csv_tables_iou(tables[i], tables[j]);
            count++;
        }
        scores[i] = (count > 0) ? (total / count) : 0.0;
    }
    return scores;
}

double judge_analysis_text(
    const std::string& prompt,
    const std::string& sql_query,
    const std::string& data,
    const std::string& analysis,
    std::shared_ptr<agents::Context> context
) {
    if (analysis.empty()) return 0.0;

    std::string truncated_data = (data.size() > 2000) ? data.substr(0, 2000) + "\n...[truncated]" : data;

    std::ostringstream oss;
    oss << "You are an expert evaluator assessing a data analysis response.\n"
        << "For the evaluation is important you consider the information that was available for the analysis, "
        << "if the SQL result is wrong or has missing data, this problem shouldn't affect the analysis score.\n\n"
        << "### CONTEXT\n"
        << "USER QUESTION: " << prompt << "\n"
        << "SQL QUERY: " << sql_query << "\n"
        << "SQL RESULTS:\n" << truncated_data << "\n\n"
        << "### ANALYSIS TO EVALUATE\n" << analysis << "\n\n"
        << "### EVALUATION RUBRIC (Rate 1-5 for each)\n\n"
        << "**CORRECTNESS (1-5)**\n"
        << "Does the analysis accurately interpret the SQL results? Are numerical values correct?\n"
        << "[1=Wrong, 3=Mostly correct, 5=Perfect]\n\n"
        << "**COMPLETENESS (1-5)**\n"
        << "Does it fully address all parts of the user's question using available data?\n"
        << "[1=Incomplete, 3=Main points covered, 5=Comprehensive]\n\n"
        << "**FAITHFULNESS (1-5)**\n"
        << "Does it only use information from SQL results? No hallucinated facts?\n"
        << "[1=Major hallucinations, 3=Minor issues, 5=Fully grounded]\n\n"
        << "### OUTPUT\n"
        << "Return ONLY valid JSON:\n"
        << "{\n"
        << "  \"correctness\": {\"score\": <1-5>, \"reasoning\": \"<brief>\"},\n"
        << "  \"completeness\": {\"score\": <1-5>, \"reasoning\": \"<brief>\"},\n"
        << "  \"faithfulness\": {\"score\": <1-5>, \"reasoning\": \"<brief>\"}\n"
        << "}";

    try {
        auto llm = context->getLLM();
        auto orig_opts = llm->getOptions();
        auto eval_opts = orig_opts;
        eval_opts.temperature = 0.2;
        eval_opts.max_tokens = 1000;
        llm->setOptions(eval_opts);

        auto response = llm->chat(oss.str());
        llm->setOptions(orig_opts);

        std::string raw = response.content;
        size_t start = raw.find('{');
        size_t end = raw.rfind('}');
        if (start != std::string::npos && end != std::string::npos && end > start) {
            JsonObject j = JsonObject::parse(raw.substr(start, end - start + 1));
            double s1 = j.value("correctness", JsonObject::object()).value("score", 1.0);
            double s2 = j.value("completeness", JsonObject::object()).value("score", 1.0);
            double s3 = j.value("faithfulness", JsonObject::object()).value("score", 1.0);
            double avg = (s1 + s2 + s3) / 3.0;
            return std::clamp((avg - 1.0) / 4.0, 0.0, 1.0);
        }
    } catch (const std::exception& e) {
        std::cerr << "Judge evaluation error: " << e.what() << "\n";
    }
    return 0.0;
}

double judge_visualization(
    const std::string& visualization_goal,
    const JsonObject& chart_config,
    const std::string& chart_code,
    const std::string& data_sample,
    std::shared_ptr<agents::Context> context
) {
    if (chart_code.empty()) return 0.0;

    std::string sample = (data_sample.size() > 1500) ? data_sample.substr(0, 1500) + "\n...[truncated]" : data_sample;
    std::string code = (chart_code.size() > 2000) ? chart_code.substr(0, 2000) : chart_code;

    std::ostringstream oss;
    oss << "You are an expert data visualization evaluator. Assess the quality of a generated visualization based on the data and the user's goal. "
        << "There is NO reference visualization — evaluate standalone quality.\n\n"
        << "## VISUALIZATION GOAL\n" << visualization_goal << "\n\n"
        << "## AVAILABLE DATA SAMPLE\n" << sample << "\n\n"
        << "## GENERATED OUTPUT\n"
        << "Chart Configuration:\n" << chart_config.dump(2) << "\n\n"
        << "Chart Code:\n```python\n" << code << "\n```\n\n"
        << "## EVALUATION CRITERIA\n"
        << "Rate each criterion on a scale of 1-5:\n\n"
        << "### 1. DATA SUITABILITY (weight 0.30)\n"
        << "Is the chart type appropriate for the data structure?\n"
        << "[1=Wrong chart type for data, 3=Acceptable, 5=Ideal choice]\n\n"
        << "### 2. AXIS MAPPING (weight 0.30)\n"
        << "Are the X and Y axes using appropriate columns from the data?\n"
        << "[1=Wrong/missing columns, 3=Acceptable mapping, 5=Perfect mapping]\n\n"
        << "### 3. CODE QUALITY (weight 0.20)\n"
        << "Will the matplotlib code execute correctly and produce a readable chart?\n"
        << "[1=Would fail/unreadable, 3=Minor issues, 5=Clean and correct]\n\n"
        << "### 4. GOAL ALIGNMENT (weight 0.20)\n"
        << "Does the visualization effectively address the user's goal?\n"
        << "[1=Misses the goal, 3=Partially addresses it, 5=Fully addresses the goal]\n\n"
        << "## OUTPUT FORMAT\n"
        << "Return ONLY valid JSON:\n"
        << "{\n"
        << "  \"data_suitability\": {\"score\": <1-5>, \"reasoning\": \"<brief>\"},\n"
        << "  \"axis_mapping\": {\"score\": <1-5>, \"reasoning\": \"<brief>\"},\n"
        << "  \"code_quality\": {\"score\": <1-5>, \"reasoning\": \"<brief>\"},\n"
        << "  \"goal_alignment\": {\"score\": <1-5>, \"reasoning\": \"<brief>\"}\n"
        << "}";

    try {
        auto llm = context->getLLM();
        auto orig_opts = llm->getOptions();
        auto eval_opts = orig_opts;
        eval_opts.temperature = 0.2;
        eval_opts.max_tokens = 1000;
        llm->setOptions(eval_opts);

        auto response = llm->chat(oss.str());
        llm->setOptions(orig_opts);

        std::string raw = response.content;
        size_t start = raw.find('{');
        size_t end = raw.rfind('}');
        if (start != std::string::npos && end != std::string::npos && end > start) {
            JsonObject j = JsonObject::parse(raw.substr(start, end - start + 1));
            double ds = j.value("data_suitability", JsonObject::object()).value("score", 1.0);
            double am = j.value("axis_mapping", JsonObject::object()).value("score", 1.0);
            double cq = j.value("code_quality", JsonObject::object()).value("score", 1.0);
            double ga = j.value("goal_alignment", JsonObject::object()).value("score", 1.0);

            for (const auto& criterion : {"data_suitability", "axis_mapping", "code_quality", "goal_alignment"}) {
                if (j.contains(criterion) && j[criterion].is_object()) {
                    int raw_sc = j[criterion].value("score", 1);
                    std::string reason = j[criterion].value("reasoning", "");
                    std::cout << "  [vis eval] " << criterion << ": " << raw_sc << "/5 — " << reason << "\n";
                }
            }

            double total = 0.0;
            total += 0.30 * std::clamp((ds - 1.0) / 4.0, 0.0, 1.0);
            total += 0.30 * std::clamp((am - 1.0) / 4.0, 0.0, 1.0);
            total += 0.20 * std::clamp((cq - 1.0) / 4.0, 0.0, 1.0);
            total += 0.20 * std::clamp((ga - 1.0) / 4.0, 0.0, 1.0);
            return total;
        }
    } catch (const std::exception& e) {
        std::cerr << "No-GT visualization judge error: " << e.what() << "\n";
    }
    return 0.0;
}

} // namespace agent_cpp
