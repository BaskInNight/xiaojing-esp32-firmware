# Documentation

This directory contains project design documents.

## Recommended Documents

| File | Content |
|------|---------|
| `hardware.md` | Hardware connections, GPIO assignments, power supply details |
| `software_arch.md` | Software architecture, component relationships, task structure |
| `pin_map.md` | GPIO resource table, peripheral pin assignments |

## Xiaojing IDF Refactor

The complete Xiaojing washing-machine requirements, architecture, interfaces, development plan, test plan, and Claude Code prompts are located at:

- [`xiaojing_idf_design/00_README_文档索引.md`](./xiaojing_idf_design/00_README_文档索引.md)
- [`xiaojing_idf_design/00_CLAUDE_CODE_使用Prompt.md`](./xiaojing_idf_design/00_CLAUDE_CODE_使用Prompt.md)

## How to Use

When starting a new peripheral test or feature:

1. Check `pin_map.md` to verify GPIO availability
2. Check `hardware.md` for wiring and power requirements
3. Check `software_arch.md` for existing component interfaces
4. Update these documents when adding new hardware or changing architecture

## Template

New documents should follow this structure:

```markdown
# Document Title

## Overview
Brief description of what this document covers.

## Details
Content organized by logical sections.

## Revision History
| Date | Change | Author |
|------|--------|--------|
| YYYY-MM-DD | Initial version | — |
```
