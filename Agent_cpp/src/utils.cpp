#include "utils.h"
#include <iostream>
#include <sstream>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <vector>
#include <numeric>

namespace agent_cpp{

void replace_all(std::string& str, const std::string& from, const std::string& to){
    if(from.empty()) 
        return;
    size_t start_pos = 0;
    while((start_pos = str.find(from, start_pos)) != std::string::npos){
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
}

std::string query_result_to_csv(duckdb_result& result){
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

bool init_database(duckdb_connection con, const DatabaseSchema& schema){
    for (const auto& table : schema.tables) {
        std::string query = "CREATE TABLE " + table.name + " AS SELECT * FROM read_parquet('" + table.file_path + "');";
        duckdb_result res;
        if (duckdb_query(con, query.c_str(), &res) == DuckDBError) {
            std::cerr << "[LookupSalesData] Errore caricamento tabella " << table.name 
                      << ": " << duckdb_result_error(&res) << std::endl;
            duckdb_destroy_result(&res);
            return false;
        }
        duckdb_destroy_result(&res);
    }
    return true;
}

// La distanza di Levenshtein calcola il numero minimo di operazioni a singolo carattere 
// necessarie per trasformare una stringa nell'altra. Viene calcolata tramite un algoritmo di 
// programmazione dinamica: costruendo una matrice delle distanze prefisse, si trova il costo minimo complessivo
double sequence_similarity(const std::string& s1, const std::string& s2) {
    if (s1 == s2) 
        return 1.0;
    if (s1.empty() || s2.empty()) 
        return 0.0;

    // Ottimizzazione: assicuriamo che s2 sia la stringa più corta per usare meno memoria
    const std::string& a = (s1.size() >= s2.size()) ? s1 : s2;
    const std::string& b = (s1.size() >= s2.size()) ? s2 : s1;

    const size_t n = a.size();
    const size_t m = b.size();

    std::vector<size_t> dp(m + 1);
    std::iota(dp.begin(), dp.end(), 0);

    for (size_t i = 1; i <= n; ++i) {
        size_t prev = dp[0]; 
        dp[0] = i;

        for (size_t j = 1; j <= m; ++j) {
            size_t temp = dp[j]; 
            if (a[i - 1] == b[j - 1])
                dp[j] = prev;
            else
                dp[j] = 1 + std::min({prev, dp[j], dp[j - 1]});
            prev = temp;
        }
    }

    size_t dist = dp[m];
    size_t max_len = std::max(s1.size(), s2.size());
    return 1.0 - (static_cast<double>(dist) / static_cast<double>(max_len));
}


}