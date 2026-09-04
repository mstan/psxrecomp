# PRINCIPLES.md
Global Debugging & Reverse Engineering Principles

This document is the SINGLE source of truth for all rules.
All other files reference this. Nothing overrides this.

---

# 1. CORE PHILOSOPHY

We do not guess.
We do not explore blindly.
We do not fix symptoms.

We identify:
1. The exact point of divergence
2. The exact state difference
3. The exact instruction or function responsible

Then we fix that — and only that.

---

# 2. STATE OVER THEORY

If two systems behave differently, then their state differs.

All debugging reduces to:
- Capturing state
- Comparing state
- Finding the first difference

Do not theorize causes without state evidence.

---

# 3. FIRST DIVERGENCE (CRITICAL)

Never debug the final symptom.

Always find:
> The FIRST moment where expected != actual

If you are not identifying the first divergence, you are doing it wrong.

---

# 4. TEMPORAL DEBUGGING

Bugs are about WHEN, not WHAT.

Reason in time:
- What happened before this?
- What changed?

---

# 5. WRITE VS READ BUGS

Determine the class:

Write-time:
- Wrong data written

Read-time:
- Correct data, wrong usage

Do not mix these.

---

# 6. TRACE THE WRITER

When state differs, find:
- WHO wrote it
- WHEN it was written
- WHY it differs

---

# 7. FIX THE SOURCE

Invalid:
- Clamping
- Skipping logic
- Hardcoding values

Valid:
- Fixing the producing logic
- Fixing execution order
- Reproducing missing state

---

# 8. MINIMAL FIXES

The correct fix:
- Smallest possible change
- Matches original system behavior

---

# 9. STRUCTURED DATA ONLY

Use:
- Ring buffers
- State snapshots
- Structured JSON artifacts

Avoid:
- printf spam
- unstructured logs
- log files of any kind

---

# 10. BUILD TOOLS, NOT GUESSWORK

If you cannot answer something:
-> build a tool to answer it

---

# 11. NEVER DEBUG BLIND

If you say:
- "maybe"
- "likely"

You are missing data.

Stop and gather it.

---

# 12. STUBS (ABSOLUTE RULE)

NO STUBS — EVER

If execution reaches unknown code:
1. STOP
2. Identify target
3. Fix discovery/codegen

Never simulate behavior.

---

# 13. FUNCTION DISCOVERY

A dispatch miss is a graph failure.

Fix:
- function finder
- codegen

Never patch output.

---

# 14. SUCCESS DEFINITION

A bug is fixed only when:
1. Root cause identified
2. Divergence explained
3. Fix addresses cause
4. Behavior matches reference

---

# 15. DISTRUST TOOLING

At the start of every session, validate that tools are doing what you
think they're doing.  Run a known-good query, check the output by hand,
verify file paths resolve where you expect.

Never trust:
- That a previous session's tool still works the same way
- That generated output matches what you asked for
- That a grep/awk pipeline found all matches

When you build a new tool or instrument, verify its FIRST output manually
before relying on it for analysis.

---

# 16. ORACLES

Truth comes from two sources:

- Ghidra for what the code is supposed to do (static analysis)
- DuckStation for what real PS1 hardware does at runtime (dynamic oracle)

Use both, never just one. Don't guess. Don't say "probably". If you
cannot answer a question from Ghidra or DuckStation, the answer is
"I don't know yet" — not a confident guess.
