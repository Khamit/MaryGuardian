# MaryGuardian

[![License: AGPL v3](https://img.shields.io/badge/License-AGPL_v3-blue.svg)](LICENSE)

**Local-first document sorter and assistant for accountants, bookkeepers and small legal practices.**
Point it at a folder of PDFs, spreadsheets and text files. It indexes them, recognises invoices, contracts,
statements and more, tracks expiry and due dates, finds duplicates, and answers questions about your library —
using a language model that runs **on your own machine**. Nothing is uploaded anywhere.

*Читать на русском: [README.ru.md](README.ru.md)*

> **Status: early / minimal working core.** It works end to end on macOS (Apple Silicon). Linux support, tests and
> packaging are open for contributions — see [Roadmap](#roadmap--help-wanted).
> MaryGuardian is an assistant, not an accountant or a lawyer: AI output can be wrong. Verify before relying on it.

## What it does

- **Fast, incremental inbox scan.** Files are identified by SHA-256, so renames, moves and duplicates are detected.
  Unchanged files are recognised from an in-memory index without being reopened; new files are hashed in parallel
  and written to SQLite in batched transactions. Re-scanning a library of 30k files takes a fraction of a second.
- **Classification in two steps.** Deterministic rules first (dates, document numbers, amounts, currency, company,
  expiry), then a local LLM for type, issuer and a one-line summary. Model answers are cached, so reclassifying
  does not repeat work. A company name from the model is accepted only if it appears verbatim in the text.
- **18 document types → collections** (invoices, receipts, credit notes, quotes, purchase orders, contracts, bank
  statements, payroll, tax, insurance, utilities, legal, identity, medical, certificates, reports, letters, other).
  Add your own collections with keywords. Empty collections are hidden in the sidebar.
- **Expiry tracking and cleanup.** Expired / expiring-in-30-days documents, possible duplicates, documents that need
  review, files that disappeared from the inbox. Files are **never modified or deleted** by the app.
- **Ask your library.** Questions are answered from document cards only (retrieval happens in code, the model cannot
  see anything else) with `[id]` citations.
- **Single-document drafting.** Extract → draft (e.g. a complaint letter) → human review → approve → export.
- **Activity screen.** Shows how much work was avoided by caching: files not re-read, hashes, AI calls vs cache hits,
  database commits.

### Supported inputs

| Input | How |
|---|---|
| PDF with a text layer | MuPDF |
| Text, CSV, XML, JSON (UTF-8 / UTF-16 / Windows-1251) | built-in reader |
| DOCX, XLSX, PPTX, ODT, ODS | built-in reader (ZIP + XML via MuPDF's archive API) |
| Scanned PDFs | optional OCR with a local vision-language model (`--ocr-model`, `--ocr-mmproj`); results are always flagged for review |
| Standalone images (JPG/PNG), legacy `.doc`/`.xls` | **not supported yet** — good first issues |

## Safety model

`Guardian` is the single checkpoint between the model and the user:

- **Default-deny** action whitelist; reads confined to `inbox/`, writes to `output/`; symlinks and oversized files refused.
- **PII detection** (cards with Luhn check, SSN, IBAN, e-mail, RU tax IDs, …) before anything is shown.
- **Prompt-injection heuristics** on document text; document text is neutralised before it enters a prompt.
- **Human-in-the-loop:** drafts must be approved before they can be exported.
- **Audit log** of every decision in `logs/audit.jsonl`.
- The web UI listens on **127.0.0.1 only**, requires a per-run token, checks `Host` and `Origin`, and sends a strict CSP.

These are heuristics, not guarantees. See [SECURITY](#reporting-security-issues).

## Quick start (macOS, Apple Silicon)

Requirements: Xcode command-line tools, Homebrew `mupdf`, SQLite (system), a build of
[llama.cpp](https://github.com/ggml-org/llama.cpp) with `mtmd` in `third_party/llama.cpp`, and
`nlohmann/json.hpp` in `third_party/nlohmann/`.

```bash
brew install mupdf
git clone --recurse-submodules https://github.com/Khamit/MaryGuardian.git && cd MaryGuardian

# llama.cpp (tested with commit: <FILL IN>) — the API changes often, pin the version you use
cd third_party/llama.cpp && cmake -B build -DGGML_METAL=ON && cmake --build build --config Release -j && cd ../..

make                 # builds ./MaryGuardian
make install-web     # copies web/index.html into mg_workspace/web
```

The UI uses [Bootstrap Icons](https://icons.getbootstrap.com/) (MIT). Put them in
`mg_workspace/web/vendor/bootstrap-icons/` (the page works without them, it just looks plainer).

```bash
# without a model: scanning, rules, search, collections, cleanup all work
./MaryGuardian --workspace mg_workspace

# with a model (any ChatML-style instruct GGUF; developed with Qwen)
./MaryGuardian --workspace mg_workspace --model /path/to/model.gguf --port 8080

# optional OCR for scanned PDFs
./MaryGuardian --workspace mg_workspace --model /path/to/model.gguf \
               --ocr-model /path/to/ocr.gguf --ocr-mmproj /path/to/mmproj.gguf
```

Put files into `mg_workspace/inbox/`, open <http://localhost:8080>, press **Scan inbox**.
Model weights are **not** included and have their own licenses.

> **Privacy:** `mg_workspace/` contains your documents' metadata, summaries and the audit log. Never commit it.
> The provided `.gitignore` excludes it.

## Project layout

```
MaryGuardian/
├── main.cpp                  entry point, argument parsing
├── Makefile
├── core/
│   ├── Guardian.*            action whitelist, path confinement, PII, approval state, audit log
│   ├── Safety.hpp            allowed actions, file sniffing, PII patterns
│   ├── Verifier.*            prompt-injection heuristics
│   ├── Db.*                  SQLite wrapper: statement cache, batched transactions, RAM caches
│   ├── Indexer.*             scan → classify → OCR pipeline, RAM index, run statistics
│   ├── Extract.*             rule-based field extraction + the document-type table
│   ├── DocReader.*           text from Office / text / XML files
│   ├── PdfToolEngine.*       MuPDF: read, render, edit
│   ├── ModelRuntime.*        llama.cpp text generation
│   ├── OcrEngine.*           vision-language OCR (llama.cpp mtmd)
│   └── Library.*             user operations: list, edit, collections, cleanup, ask
├── task/BookkeeperTask.*     single-document extract → draft → approve flow
├── server/HttpServer.*       loopback HTTP server and JSON API
├── tests/guardian_test.cpp   `make test`
└── web/index.html            single-page UI
```

Workspace (created at runtime): `inbox/`, `output/`, `logs/audit.jsonl`, `library.db`, `web/`.

## Design principle

> Don't ask how to do the work faster. Ask whether it must be done at all — then once, then ahead of time, then only
> for what changed, then at a cheaper layer of the system — and only then optimise the operation itself.

Concretely: unchanged files are never reopened, SHA-256 is computed once per file state, SQL is persistence rather
than a working set, writes are batched, and model answers are cached. Please keep this in mind when contributing.

## HTTP API

All `/api/*` calls need the `X-Auth-Token` header (embedded in the served page). JSON in, JSON out.

```
GET  /                      UI
GET  /api/status            single-document flow state, model loaded
GET  /api/audit             Guardian log
POST /api/upload|draft|approve|edit|export      single-document flow
POST /api/lib/scan          start a scan
POST /api/lib/scan_status   progress, report, run statistics, data revision
POST /api/lib/list|doc|update|collections|collection_save|collection_delete|resort
POST /api/lib/companies|company_rename|profile_get|profile_set|cleanup|ask
```

## Roadmap / help wanted

- **Linux port:** replace CommonCrypto SHA-256 (OpenSSL or a small bundled implementation), CPU/CUDA backends, CMake.
- Accept **standalone images** (JPG/PNG) — the file sniffer currently rejects them before OCR.
- **Filesystem watcher** (FSEvents / inotify) instead of manual rescans.
- Language packs: date formats, company forms, document keywords for DE / FR / ES / RU …; localised UI and prompts.
- **Tests** for `Extract` rules, `Indexer` scan logic and `Db`.
- Legacy `.doc` / `.xls` (OLE2), `.eml` / `.msg`.
- Packaging (`.app` / notarised `.dmg`), export collections to a folder tree.

**Adding a document category is one line** in `docTypes()` in `core/Extract.hpp`
(plus optional rules in `Extract.cpp`). The model prompt, validation, collections and UI pick it up automatically.

## Contributing

Issues and pull requests are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md). Changes to `Guardian`, `Safety`,
or anything that touches files need extra care and a test.

### Reporting security issues

Please do **not** open a public issue for vulnerabilities. Use GitHub's private
"Report a vulnerability" feature on this repository, or e-mail `<gercules789@gmail.com>`.

## License

MaryGuardian is licensed under the **GNU Affero General Public License v3.0 or later** — see [LICENSE](LICENSE).
Copyright © `<2026> <Khamit>`.

It links [MuPDF](https://mupdf.com/), which is itself AGPL-3.0 (with a separate commercial license from Artifex),
so the combined program is distributed under the AGPL. Other components:

| Component | License |
|---|---|
| [llama.cpp](https://github.com/ggml-org/llama.cpp) (incl. ggml, mtmd) | MIT |
| [MuPDF](https://mupdf.com/) | AGPL-3.0-or-later / Artifex commercial |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT |
| [SQLite](https://sqlite.org/) | Public domain |
| [Bootstrap Icons](https://icons.getbootstrap.com/) | MIT |
| Model weights (not included) | per model — check before use or redistribution |

"MaryGuardian" and its logo are not licensed for use as the name of a derived product without permission.