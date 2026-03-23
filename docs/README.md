# How This Project Was Built — AI-Assisted Firmware Development

## The Story

I got kicked out of the GP2040-CE Discord. A big part of why — from what I could tell — was that I used AI to help write code. The implication was that using AI meant the work wasn't legitimate, that I didn't really understand what I was building, or that I was somehow cheating.

I'm not a firmware engineer. I'm not an embedded systems expert. I don't have a computer science degree. But I had a problem I wanted to solve — reliable simultaneous button presses for fighting games — and later, native Dreamcast controller support without adapters. These are hard embedded firmware problems involving RP2040 PIO state machines, DMA ring buffers, bit-level protocol implementation, and real-time constraints measured in microseconds.

I used AI to help me build it. Specifically, [Claude Code](https://claude.com/claude-code) — Anthropic's CLI agent. And I'm proud of it.

The NOBD sync window works. The Dreamcast Maple Bus driver works. I tested them on real hardware, debugged them on real hardware, and shipped them. The AI didn't do that part — I did. But the AI helped me understand PIO programming, Maple Bus protocol specs, byte ordering pipelines, and dozens of other things I would have spent months learning from scratch.

**This docs folder exists to show you exactly how I did it**, so you can do it too.

---

## What Is Claude Code?

[Claude Code](https://claude.com/claude-code) is a CLI tool from Anthropic that gives Claude (an AI model) access to your codebase. It can read files, search code, run builds, edit files, and have extended conversations about your project. It runs in your terminal and works with any codebase.

Key things it can do:
- Read and understand your entire codebase
- Search for patterns across files (like `grep` but with understanding)
- Edit files with precise replacements
- Run shell commands (builds, tests, git)
- Maintain context across a long conversation
- Use "memory" files to persist knowledge between sessions
- Spawn sub-agents for parallel research tasks

It's not autocomplete. It's not a chatbot. It's an agent that can take multi-step actions in your codebase.

---

## How We Used It On This Project

### 1. The CLAUDE.md Context File

The most important file in this repo for AI-assisted development is [`CLAUDE.md`](../CLAUDE.md) in the project root. Claude Code automatically loads this file at the start of every conversation. It contains:

- What the project is and what it does
- Architecture overview (where code lives, how it flows)
- File map (which files do what)
- Build instructions
- Configuration pipeline (proto → defaults → webconfig → web UI)
- Common tasks and how to do them

**This is the secret sauce.** A well-written CLAUDE.md means the AI starts every conversation already understanding your project. Without it, you'd spend half your time re-explaining basics.

#### How to write a good CLAUDE.md:
- Start with "what is this project" in 2-3 sentences
- List every file that's been modified from upstream, with one-line descriptions
- Include build commands (especially non-obvious ones)
- Document the data flow (where does input come from, where does output go)
- Include domain-specific knowledge the AI wouldn't know (e.g., "Maple Bus uses inverted button logic: 0=pressed")
- Update it as the project evolves

### 2. Memory Files

Claude Code has a persistent memory system at `~/.claude/projects/<project>/memory/`. Memory files survive between conversations — when you close the terminal and come back days later, Claude remembers what you were working on.

We used memory files to store:
- **Expert context** for the Dreamcast Maple Bus protocol — architecture, PIO details, byte ordering, known bugs, debugging history
- **Architecture options** we evaluated (7 different approaches for Maple Bus RX)
- **What worked and what didn't** — so we didn't repeat failed approaches

Memory is organized as markdown files with frontmatter (name, description, type). A `MEMORY.md` index file links them together. Claude reads relevant memories at the start of conversations.

### 3. Plan Files

For complex multi-step tasks, we used Claude's plan mode (`/plan`). This creates a structured plan file that persists across the conversation. For the Dreamcast driver, our plan covered:
- Root cause analysis of the disconnect cycling bug
- Prioritized fix list (which bugs to tackle first)
- File-by-file change descriptions
- Verification steps

Plans keep both you and the AI aligned on what's being done and why.

### 4. Expert Analysis Documents

When we hit a wall with the Dreamcast disconnect cycling, we had Claude do a deep comparative analysis of our implementation vs. the reference implementation (MaplePad/DreamPicoPort). This produced a document ranking every bug by severity and likelihood.

The top-ranked bug (PIO state machines not properly restarted after TX) turned out to be the actual root cause. The analysis document identified it before we even started debugging — we just had to work through the list.

### 5. Sub-Agents for Parallel Research

Claude Code can spawn sub-agents — independent research tasks that run in parallel. We used these to:
- Search the MaplePad reference implementation for specific protocol details
- Look up Dreamcast controller port pinouts
- Check community forums for related work
- Explore the codebase for all references to a function

This is faster than doing sequential searches and keeps the main conversation focused.

### 6. Iterative Build-Test-Debug Cycles

The typical workflow for a firmware change:
1. Describe the problem or feature to Claude
2. Claude reads the relevant files, proposes changes
3. Review the changes, approve edits
4. Claude runs the build (`build_one.bat`)
5. Flash the UF2 to hardware, test on real Dreamcast
6. Report results back to Claude ("CP=8, still cycling")
7. Claude adjusts approach based on real-world feedback
8. Repeat

The AI can't test on hardware — that's your job. But it can iterate on code changes very quickly based on your test results. The Dreamcast driver went through dozens of iterations this way.

---

## How to Get Started With Claude Code on This Project

### Prerequisites
- [Claude Code CLI](https://claude.com/claude-code) installed
- This repo cloned locally
- ARM GCC toolchain + CMake + Ninja (for building)
- Node.js + npm (for web UI changes)

### Your First Session

```bash
cd /path/to/GP2040-CE
claude
```

Claude will automatically load `CLAUDE.md` and any relevant memory files. Try:

- **"What does the NOBD sync window do?"** — Claude will explain the algorithm by reading the actual code
- **"Show me how the Dreamcast driver processes a GET_CONDITION command"** — Claude will trace through the code path
- **"What PIO programs does the Maple Bus use?"** — Claude will read `maple.pio` and explain each state machine
- **"Build the firmware for RP2040AdvancedBreakoutBoard"** — Claude will run the build command

### Making Changes

```
"Add a new button mapping for the Dreamcast driver that maps S1 to DC Start"
```

Claude will:
1. Find the button mapping function in `DreamcastDriver.cpp`
2. Show you the current mappings
3. Propose the edit
4. After you approve, run the build to verify it compiles

### Debugging With Hardware Feedback

```
"I flashed the firmware and the OLED shows XF:5 and the controller disconnects every 2 seconds.
XF was 0 before my last change. What could cause XOR failures?"
```

Claude will analyze what changed, cross-reference with the protocol spec, and suggest specific things to check. This is where the expert context in CLAUDE.md and memory files pays off — Claude already knows the architecture.

### Creating Your Own Expert Context

If you're working on a new feature:

1. **Research phase:** Ask Claude to analyze relevant reference code, protocol specs, or existing implementations. Save the analysis to a memory file.
2. **Planning phase:** Use `/plan` to create a structured approach before writing code.
3. **Implementation phase:** Work through the plan step by step, building and testing between steps.
4. **Documentation phase:** After it works, ask Claude to update CLAUDE.md and memory files with what was learned.

---

## What the AI Can and Can't Do

### What it's great at:
- Reading and understanding large codebases quickly
- Finding patterns, references, and dependencies across files
- Explaining protocols, byte ordering, and bit manipulation
- Generating correct C/C++ code for embedded systems
- Running builds and catching compile errors
- Maintaining context across long debugging sessions
- Comparing implementations (your code vs. reference code)

### What it can't do:
- Test on real hardware (that's you)
- Debug electrical issues (signal integrity, wiring)
- Know if the Dreamcast actually accepts a response (only hardware testing reveals this)
- Replace domain expertise entirely — you still need to understand what you're building at a high level
- Guarantee correctness — always test on hardware

### The human's job:
- Define the problem ("I want my fight stick to work on Dreamcast")
- Test on real hardware and report results accurately
- Make judgment calls ("should we disable VMU for stability?")
- Verify claims ("is the left analog actually working?")
- Ship it

---

## You Don't Need to Be a Firmware Expert

I'm not one. I'm a technically inclined person who wanted to solve a specific problem. The AI helped me bridge the gap between "I know what I want to build" and "I know how RP2040 PIO state machines work."

If you can:
- Describe what you want clearly
- Read code well enough to review changes
- Flash a UF2 and test on hardware
- Report what happened honestly ("it didn't work, here's what the screen showed")

...then you can contribute to this project. The AI handles the parts that would otherwise require years of embedded systems experience. You handle the parts that require a human with hardware in hand.

That's not cheating. That's using the tools available to you.

---

## Project Links

- **This repo:** https://github.com/t3chnicallyinclined/GP2040-CE-NOBD
- **Claude Code:** https://claude.com/claude-code
- **GP2040-CE (upstream):** https://gp2040-ce.info/
- **CLAUDE.md (our context file):** [../CLAUDE.md](../CLAUDE.md)
