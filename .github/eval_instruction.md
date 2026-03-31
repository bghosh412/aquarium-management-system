# Eval Instructions

This file defines the required eval workflow and artifact formats for this repository.

---

## ✅ Required Pipeline (Multi-Item Requests)
When the user asks to fix multiple items, follow this exact pipeline:

1. Initialize eval framework for each line item.
2. Detail requirements and acceptance criteria and update the respective `story.md`.
   Also create an implementation plan that details the required changes (file name, method name, and what to update).
   Name the implementation plan as per eval framework nomenclature.
3. Generate code.
4. Build and check for build errors.
5. Run eval and show output JSON files in chat.

---

## 📄 Story Format (story.md)
The parser expects:
- A single H1 title.
- Description immediately after the H1 (before the first H2).
- A numbered list of acceptance criteria under an H2 section named "Acceptance Criteria".

**Template:**
```
# STORY_ID

<Story description in plain text>

## Acceptance Criteria

1. First acceptance criterion
2. Second acceptance criterion
3. ...
```

---

## 🧩 Implementation Plan Format (implementation_plan.md)
The plan parser extracts files from bullet lines containing paths. Use the exact format below:

**Template:**
```
# Implementation Plan: STORY_ID

## Summary
<Short summary>

## Planned Changes
- path/to/file.ext
  - `MethodOrFunction()`
    - What will be updated (verbal)
    - ...
```

**Notes:**
- File paths must appear directly after the dash (e.g., `- src/main.cpp`).
- Methods/functions should be backticked so they are captured by the plan parser.

---

## 🧾 Code Diff Format (code_diff.patch)
Provide a unified diff. If generated via git, use:
```
# Example
# git diff -- path/to/file > .eval/artifacts/<story>/code_diff.patch
```

---

## 📂 Standard Artifact Paths
```
.eval/
└── artifacts/
    └── <story-id>/
        ├── story.md
        ├── implementation_plan.md
        ├── code_diff.patch
        ├── generated/
        ├── tests/
        └── context/
```
