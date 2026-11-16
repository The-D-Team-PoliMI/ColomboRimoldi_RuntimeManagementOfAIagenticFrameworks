#!/usr/bin/env python3
"""
Benchmark del progetto C++ con le stesse GT e gli stessi evaluator del progetto Python.

Per ogni entry selezionata di benchmark_dataset*.json:
  1. lancia l'eseguibile C++   (--prompt, --goal, --save, --run-id benchNN)
  2. legge la run salvata in cache dal C++
  3. la valuta con evaluate_cpp_run.evaluate(), che chiama make_csv/text/vis_evaluator_gt
     di Agent/utils.py (nessun porting)
Scrive results.json e results.csv in <cwd>/Evaluation (cioe' Agent_cpp/Evaluation).

Selezione dei prompt (numerati da 1, come "PROMPT 1/20" nel benchmark Python):
    --max-prompts 5          i primi 5
    --prompts 1-5            gli stessi, come intervallo
    --prompts 14-18          solo dal 14 al 18
    --prompts 2 4 7-9        prompt 2, 4, 7, 8, 9
Senza nessuna delle due: tutti i prompt.

Esempio (Gemini sia come agente C++ sia come giudice GT):
    export OPENAI_API_KEY="<chiave Gemini>"
    export OPENAI_BASE_URL="https://generativelanguage.googleapis.com/v1beta/openai/"
    python run_cpp_benchmark.py evaluation/benchmark_dataset.json \
        --exe ../Agent_cpp/build/agent_cpp --cwd ../Agent_cpp --utils Agent/utils.py \
        --judge-provider openai --judge-model gemini-3.5-flash-lite \
        --prompts 1-5 --run-name gemini_test
    -> risultati in ../Agent_cpp/Evaluation/gemini_test/
"""
import argparse
import csv
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import evaluate_cpp_run as ev  # noqa: E402


# Errori dell'API del modello che il C++ stampa (es. 'Gemini API error: 429 | Too Many Requests').
API_ERROR_RE = re.compile(r"[^\n]*(API error|Too Many Requests|RESOURCE_EXHAUSTED|\b429\b)[^\n]*", re.I)


def run_and_stream(cmd, cwd, timeout, echo=True):
    """Lancia il C++ e ne stampa l'output riga per riga mentre gira (stdout e stderr insieme).

    Ritorna (returncode, output completo). Su timeout il processo viene terminato e rc = -1.
    """
    # Con l'output su pipe il C++ bufferizza std::cout e le righe arriverebbero tutte alla fine:
    # stdbuf -oL forza il flush a ogni riga (std::cout e' sincronizzato con stdio).
    if shutil.which("stdbuf"):
        cmd = ["stdbuf", "-oL", "-eL", *cmd]
    proc = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, bufsize=1, errors="replace")
    timed_out = threading.Event()

    def _kill():
        timed_out.set()
        proc.kill()

    timer = threading.Timer(timeout, _kill)
    timer.start()
    lines = []
    try:
        for line in proc.stdout:
            lines.append(line)
            if echo:
                print("   | " + line, end="", flush=True)
        proc.wait()
    finally:
        timer.cancel()
    if timed_out.is_set():
        lines.append(f"\ntimeout dopo {timeout} s\n")
        return -1, "".join(lines)
    return proc.returncode, "".join(lines)


def first_score(report, step):
    items = report.get(step) or []
    return items[0]["score"] if items else None


# Nomi delle colonne uguali a quelli di benchmark_results.csv del benchmark Python.
_STEP_COLUMNS = {
    "lookup_sales_data": "lookup",
    "analyzing_data": "analyzing",
    "create_visualization": "vis",
    "decide_tool": "decide",
}


def read_timings(run_dir: Path) -> dict:
    """Tempi misurati dal C++ (timing.h), letti dallo stato finale salvato in metadata.json.

    elapsed_sec = _total_run_time_sec (come elapsed_sec in Python); per step: tempo totale dello
    step e tempo delle sole chiamate LLM. decide_tool e' l'ultima chiamata (o la somma, se hai
    attivato la variante che accumula).
    """
    meta = json.loads((run_dir / "metadata.json").read_text(encoding="utf-8"))
    state = meta.get("final_result") or {}
    step_t = state.get("_step_timings_sec") or {}
    llm_t = state.get("_step_llm_timings_sec") or {}
    out = {"elapsed_sec": state.get("_total_run_time_sec")}
    for step, short in _STEP_COLUMNS.items():
        out[f"{short}_time_sec"] = step_t.get(step)
        out[f"{short}_llm_time_sec"] = llm_t.get(step)
    return out


TIME_COLUMNS = ["elapsed_sec"] + [f"{s}_time_sec" for s in _STEP_COLUMNS.values()] \
    + [f"{s}_llm_time_sec" for s in _STEP_COLUMNS.values()]


def parse_prompt_selection(tokens, total):
    """Trasforma ["2", "4", "7-9"] in indici 0-based [1, 3, 6, 7, 8] (numerazione utente da 1)."""
    selected = []
    for tok in tokens:
        for part in tok.split(","):
            part = part.strip()
            if not part:
                continue
            if "-" in part:
                a, b = part.split("-", 1)
                lo, hi = int(a), int(b)
            else:
                lo = hi = int(part)
            if lo < 1 or hi > total or lo > hi:
                raise SystemExit(f"Selezione non valida '{part}': i prompt vanno da 1 a {total}")
            selected.extend(range(lo - 1, hi))
    return sorted(set(selected))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dataset", help="benchmark_dataset*.json (stesso file usato in Python)")
    ap.add_argument("--exe", required=True, help="eseguibile C++ compilato")
    ap.add_argument("--cwd", default=".", help="cartella Agent_cpp: da qui parte il C++ (data/, cache/ relativi)")
    ap.add_argument("--cache-dir", help="cache del C++ (default: <cwd>/cache/agent_runs)")
    ap.add_argument("--utils", default="Agent/utils.py", help="Agent/utils.py del progetto Python")
    ap.add_argument("--judge-provider", default="openai", choices=["openai", "ollama"])
    ap.add_argument("--judge-model", default="gpt-4o-mini")
    ap.add_argument("--ollama-url", default="http://localhost:11434")
    ap.add_argument("--text-metric", default="judge_gt", choices=["judge_gt", "bleu", "spice"])
    ap.add_argument("--skip", nargs="*", default=[], choices=["csv", "text", "vis"])
    ap.add_argument("--deterministic", action="store_true",
                    help="nessun modello AI per la valutazione: CSV con compare_dataframes_similarity_gt, "
                         "testo con bleu_score, visualizzazione non valutata (in Python esiste solo il giudice LLM)")
    sel = ap.add_mutually_exclusive_group()
    sel.add_argument("--max-prompts", type=int, help="esegue solo i primi N prompt")
    sel.add_argument("--prompts", nargs="+", metavar="N|A-B",
                     help="prompt da eseguire, numerati da 1 (es. --prompts 1-5 oppure 2 4 7-9)")
    ap.add_argument("--timeout", type=int, default=900, help="secondi massimi per prompt")
    ap.add_argument("--quiet", action="store_true",
                    help="non stampare l'output del C++ (resta comunque nei file benchNN.log)")
    ap.add_argument("--sleep", type=float, default=0.0,
                    help="secondi di pausa tra un prompt e il successivo (utile con i limiti di Gemini)")
    ap.add_argument("--retries", type=int, default=2,
                    help="tentativi in piu' su errore API del modello, es. 429 (default: 2)")
    ap.add_argument("--retry-wait", type=float, default=60.0,
                    help="attesa prima di riprovare, moltiplicata per il numero del tentativo (default: 60 s)")
    ap.add_argument("--resume", action="store_true",
                    help="salta i prompt gia' riusciti in results.json (quelli falliti vengono rifatti)")
    ap.add_argument("--run-name", default="cpp_benchmark",
                    help="sottocartella di <cwd>/Evaluation in cui salvare (default: cpp_benchmark)")
    ap.add_argument("--save-dir", help="cartella di output esplicita (sostituisce <cwd>/Evaluation/<run-name>)")
    args = ap.parse_args()
    if args.deterministic:
        args.text_metric = "bleu"
        args.skip = sorted(set(args.skip) | {"vis"})
    uses_llm = (args.text_metric == "judge_gt" and "text" not in args.skip) or ("vis" not in args.skip)
    if uses_llm:
        print(f"Valutazione con giudice LLM: {args.judge_provider}/{args.judge_model}")
    else:
        print(f"Valutazione senza LLM: csv={'no' if 'csv' in args.skip else 'si'}, "
              f"testo={'no' if 'text' in args.skip else args.text_metric}, vis=no")

    dataset = json.loads(Path(args.dataset).read_text(encoding="utf-8"))
    total = len(dataset)
    if args.prompts:
        indices = parse_prompt_selection(args.prompts, total)
    elif args.max_prompts:
        indices = list(range(min(args.max_prompts, total)))
    else:
        indices = list(range(total))

    cwd = Path(args.cwd).resolve()
    cache_dir = Path(args.cache_dir) if args.cache_dir else cwd / "cache" / "agent_runs"
    exe_path = Path(args.exe).resolve()
    if not exe_path.is_file() or not os.access(exe_path, os.X_OK):
        raise SystemExit(f"Eseguibile C++ non trovato o non eseguibile: {exe_path}\n"
                         "Compila Agent_cpp (cmake --build build) e passa il percorso giusto con --exe.")
    for label, p in (("dataset", Path(args.dataset)), ("--utils", Path(args.utils))):
        if not p.is_file():
            raise SystemExit(f"File {label} non trovato: {p.resolve()}")
    exe = str(exe_path)
    save_dir = Path(args.save_dir) if args.save_dir else cwd / "Evaluation" / args.run_name
    save_dir.mkdir(parents=True, exist_ok=True)
    results_path = save_dir / "results.json"
    print(f"Prompt selezionati: {[i + 1 for i in indices]} (su {total})")
    print(f"Risultati in: {save_dir}")

    rows = json.loads(results_path.read_text(encoding="utf-8")) if (args.resume and results_path.exists()) else []
    rows = [r for r in rows if not r.get("error")]  # con --resume i prompt falliti vengono rifatti
    done = {r["index"] for r in rows}
    utils = ev.load_utils(Path(args.utils))

    for pos, idx in enumerate(indices, start=1):
        if idx in done:
            continue
        entry = dataset[idx]
        run_id = f"bench{idx:02d}"          # indice del dataset completo: stabile tra run diverse
        prompt = entry["prompt"]
        goal = entry.get("visualization_goal") or ""
        cmd = [exe, "--prompt", prompt, "--goal", goal, "--save", "--run-id", run_id]
        print(f"\n[{pos}/{len(indices)}] PROMPT {idx + 1}/{total}: {prompt}")

        run_dir = cache_dir / run_id
        api_error = False
        for attempt in range(args.retries + 1):
            # Una run vecchia con lo stesso id non deve passare per buona se questa fallisce.
            shutil.rmtree(run_dir, ignore_errors=True)
            t0 = time.perf_counter()
            rc, output = run_and_stream(cmd, cwd, args.timeout, echo=not args.quiet)
            wall = time.perf_counter() - t0
            (save_dir / f"{run_id}.log").write_text(output, encoding="utf-8")
            api_error = bool(API_ERROR_RE.search(output))
            if not api_error or attempt == args.retries:
                break
            wait = args.retry_wait * (attempt + 1)
            print(f"   errore API del modello (limite di richieste?): riprovo tra {wait:.0f} s "
                  f"[{attempt + 1}/{args.retries}]")
            time.sleep(wait)

        row = {"index": idx, "prompt_number": idx + 1, "run_id": run_id, "prompt": prompt,
               "difficulty": entry.get("difficulty"), "returncode": rc, "wall_sec": round(wall, 3),
               "csv": None, "text": None, "vis": None, "error": None}
        meta_path = run_dir / "metadata.json"
        saved_prompt = None
        if meta_path.exists():
            try:
                saved_prompt = json.loads(meta_path.read_text(encoding="utf-8")).get("prompt")
            except Exception:
                pass
        last_line = " ".join(output.strip().splitlines()[-1].split())[:200] if output.strip() else ""
        if api_error:
            row["error"] = "errore API del modello (es. 429 Too Many Requests): " + \
                           " ".join(API_ERROR_RE.search(output).group(0).split())
        elif rc != 0:
            row["error"] = f"il C++ e' terminato con codice {rc}: {last_line}"
        elif not meta_path.exists():
            row["error"] = (f"nessuna run salvata in {run_dir}: il C++ ignora --save/--run-id? "
                            f"(aggiungi il parsing degli argomenti in main.cpp). Ultima riga: {last_line}")
        elif saved_prompt != prompt:
            row["error"] = (f"il C++ ha eseguito un altro prompt ({saved_prompt!r}): "
                            "ignora --prompt? (aggiungi il parsing degli argomenti in main.cpp)")
        else:
            report = ev.evaluate(run_dir, entry, utils, text_metric=args.text_metric,
                                 judge_provider=args.judge_provider, judge_model=args.judge_model,
                                 ollama_url=args.ollama_url, skip=args.skip)
            row["csv"] = first_score(report, "lookup_sales_data")
            row["text"] = first_score(report, "analyzing_data")
            row["vis"] = first_score(report, "create_visualization")
            row["reasoning"] = {s: [i["reasoning"] for i in items] for s, items in report.items()}
            try:
                row.update(read_timings(run_dir))
            except Exception as e:  # metadata illeggibile: i punteggi restano validi
                print(f"   (tempi non letti: {e})")
        print(f"   csv={row['csv']} text={row['text']} vis={row['vis']} "
              f"elapsed={row.get('elapsed_sec')}s wall={row['wall_sec']}s"
              + (f"  ERRORE: {row['error']}" if row["error"] else ""))
        rows.append(row)
        if args.sleep and pos < len(indices):
            time.sleep(args.sleep)
        rows.sort(key=lambda r: r["index"])
        results_path.write_text(json.dumps(rows, indent=2, ensure_ascii=False), encoding="utf-8")

    with open(save_dir / "results.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, delimiter=";")  # ';' per aprirlo in Excel con locale italiano
        w.writerow(["prompt_number", "run_id", "difficulty", "csv", "text", "vis",
                    *TIME_COLUMNS, "wall_sec", "error", "prompt"])
        for r in rows:
            w.writerow([r.get("prompt_number", r["index"] + 1), r["run_id"], r.get("difficulty"),
                        r["csv"], r["text"], r["vis"], *[r.get(c) for c in TIME_COLUMNS],
                        r["wall_sec"], r["error"], r["prompt"]])

    def mean(key):
        vals = [r[key] for r in rows if r.get(key) is not None]
        return sum(vals) / len(vals) if vals else float("nan")
    n_err = sum(1 for r in rows if r["error"])
    print(f"\nMedie su {len(rows)} prompt ({n_err} falliti): csv={mean('csv'):.3f} text={mean('text'):.3f} "
          f"vis={mean('vis'):.3f}")
    print("Tempi medi (s): " + "  ".join(f"{c}={mean(c):.2f}" for c in TIME_COLUMNS) +
          f"  wall_sec={mean('wall_sec'):.2f}")
    print(f"Risultati in {save_dir}/results.json e results.csv")


if __name__ == "__main__":
    main()