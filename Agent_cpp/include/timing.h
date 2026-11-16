#pragma once
#include <chrono>
#include <cmath>

namespace agent_cpp {

// steady_clock è monotono (come time.perf_counter() di Python): adatto a misurare intervalli.
using Clock = std::chrono::steady_clock;

// Secondi trascorsi dall'istante `start` (duration<double> converte direttamente in secondi).
inline double elapsed_sec(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

// Equivalente di round(x, 3) di Python.
inline double round3(double x) {
    return std::round(x * 1000.0) / 1000.0;
}

// Tempo speso nelle chiamate LLM dello step in corso: equivale a _LLMCallAccumulator.total_time di Python.
// Il middleware lo azzera prima di ogni step e lo legge alla fine.
inline double& llm_time_accumulator() {
    static double total = 0.0;
    return total;
}

// Esegue una chiamata LLM e ne somma la durata all'accumulatore, anche se la chiamata lancia
// un'eccezione (Python conta anche le chiamate andate in errore).
template <typename F>
auto timed_llm_call(F&& call) {
    struct Guard {
        const Clock::time_point start = Clock::now();
        ~Guard() { llm_time_accumulator() += elapsed_sec(start); }
    };

    Guard guard;
    return call();
}

}