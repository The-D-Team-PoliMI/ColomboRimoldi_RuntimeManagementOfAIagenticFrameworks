#pragma once
#include <string>
#include <duckdb.h>
#include "schema.h"

namespace agent_cpp {

/**
 * @brief Sostituisce tutte le occorrenze di una sottostringa all'interno di una stringa.
 * @param[in,out] str Stringa bersaglio da modificare.
 * @param from Sottostringa da cercare.
 * @param to Stringa sostitutiva.
 */
void replace_all(std::string& str, const std::string& from, const std::string& to);

/**
 * @brief Converte un risultato DuckDB in formato stringa CSV e lo salva in `Output/result.csv`.
 * @param result Struttura `duckdb_result` prodotta dall'esecuzione di una query.
 * @return std::string Contenuto completo della tabella in formato CSV.
 */
std::string query_result_to_csv(duckdb_result& result);

/**
 * @brief Inizializza il database DuckDB caricando in memoria le tabelle dai file Parquet definiti nello schema.
 * @param con Connessione attiva a DuckDB.
 * @param schema Schema contenente nomi e percorsi dei file Parquet.
 * @return true se tutte le tabelle sono state create correttamente, false in caso di errore SQL/IO.
 */
bool init_database(duckdb_connection con, const DatabaseSchema& schema);

/**
 * @brief Calcola la similarità normalizzata in [0.0, 1.0] tra due stringhe tramite la distanza di Levenshtein.
 * @param s1 Prima stringa da confrontare.
 * @param s2 Seconda stringa da confrontare.
 * @return double Indice di similarità (1.0 = stringhe identiche, 0.0 = completamente diverse).
 */
double sequence_similarity(const std::string& s1, const std::string& s2);

}