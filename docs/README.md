# How This Project Was Built — AI-Assisted Firmware Development

## The Story

I got kicked out of the GP2040-CE Discord. A big part of why — from what I could tell — was that I used AI to help write code. The implication was that using AI meant the work wasn't legitimate, that I didn't really understand what I was building, or that I was somehow cheating.

I'm not a firmware engineer. I'm not an embedded systems expert. I don't have a computer science degree. I'm a cloud engineer who came back to Marvel vs. Capcom 2 after a 15-year hiatus and kept dropping dashes. That sent me down a rabbit hole — first into USB polling and frame boundaries, then into building a sync window to fix it, and eventually into writing a native Dreamcast Maple Bus driver so my fight stick could work on a real DC without an adapter. These are hard embedded firmware problems involving RP2040 PIO state machines, DMA ring buffers, bit-level protocol implementation, and real-time constraints measured in microseconds.

I used AI to help me build it. Specifically, [Claude Code](https://claude.com/claude-code) — Anthropic's CLI agent. And I'm proud of it.

The NOBD sync window works — dashes come out clean in MvC2. The Dreamcast Maple Bus driver works — I played MvC2 on a real Dreamcast for 5+ minutes straight with no disconnects (first bug i was running into was constant disconnects/reconnects). I tested them on real hardware, debugged them on real hardware, and shipped them. The AI didn't do that part — I did. But the AI helped me understand PIO programming, Maple Bus protocol specs, byte ordering pipelines, and dozens of other things I would have spent months learning from scratch.

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

## How I Used It On This Project

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

I used memory files to store:
- **Expert context** for the Dreamcast Maple Bus protocol — architecture, PIO details, byte ordering, known bugs, debugging history
- **Architecture options** I evaluated with Claude (7 different approaches for Maple Bus RX)
- **What worked and what didn't** — so I didn't repeat failed approaches

Memory is organized as markdown files with frontmatter (name, description, type). A `MEMORY.md` index file links them together. Claude reads relevant memories at the start of conversations.

### 3. Plan Files

For complex multi-step tasks, I used Claude's plan mode (`/plan`). This creates a structured plan file that persists across the conversation. For the Dreamcast driver, my plan covered:
- Root cause analysis of the disconnect cycling bug
- Prioritized fix list (which bugs to tackle first)
- File-by-file change descriptions
- Verification steps

Plans keep both you and the AI aligned on what's being done and why.

### 4. Expert Analysis Documents

When I hit a wall with the Dreamcast disconnect cycling, I had Claude do a deep comparative analysis of my implementation vs. the reference implementation (MaplePad/DreamPicoPort). This produced a document ranking every bug by severity and likelihood.

The top-ranked bug (PIO state machines not properly restarted after TX) turned out to be the actual root cause. The analysis identified it before I even started debugging — I just had to work through the list.

### 5. Sub-Agents for Parallel Research

Claude Code can spawn sub-agents — independent research tasks that run in parallel. I used these to:
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
- **Be the eyes, ears, and direction.** The AI can't see your hardware. I was the one watching the OLED flash `cr:9` to `cr:1` at a steady pace and telling Claude "this is cycling at the same rate as the polling — something is wrong at the transport layer, not the protocol layer." I didn't deeply understand PIO state machines, but I could observe patterns the AI couldn't. I also found the reference repos (MaplePad, DreamPicoPort) and fed them to Claude as examples — the AI didn't go looking for those, I did. And I constantly asked qualifying questions: "what are the tradeoffs of DMA vs polling?" "why clkdiv 3 instead of 1?" "what happens if we don't restart the state machines?" That's how you build enough of a high-level picture to steer the work, even if you're not writing the code yourself.
- **Test on real hardware and report results accurately.** "CP=8, still cycling, cr flips between 1 and 9" gives the AI something to work with. "It doesn't work" doesn't.
- **Make judgment calls.** "Disable VMU for now and isolate the controller connection first" — that's triage, and only the human with hardware in hand can make that call.
- **Verify claims.** The AI will say something works. Trust but verify — flash the UF2 and test it yourself.
- **Ship it.**

---

## You Don't Need to Be a Firmware Expert

I'm not one. I'm a cloud engineer who noticed the same thing everyone else was reporting — dashes drop constantly in MvC2 on PC/Steam when they never did on Dreamcast or arcade. My tech background told me this was probably a hardware timing issue, not a skill issue, and I went down the rabbit hole to prove it. Turns out it's a real problem — USB frame boundaries split simultaneous presses that the original hardware grouped naturally. Most modern fighting games solve this with input leniency, but MvC2 is a 25-year-old arcade game that doesn't. The [main README](../README.md) has the full technical breakdown. The AI helped me bridge the gap between understanding the problem and actually implementing the fix in RP2040 firmware.

If you can:
- Describe what you want clearly
- Read code well enough to review changes
- Flash a UF2 and test on hardware
- Report what happened honestly ("it didn't work, here's what the screen showed")

...then you can contribute to this project. The AI handles the parts that would otherwise require years of embedded systems experience. You handle the parts that require a human with hardware in hand.

That's not cheating. That's using the tools available to you.

### The Real Productivity Story

AI didn't make me 10x faster — it made this *feasible*. I'm technical enough to have figured out PIO state machines and Maple Bus protocol implementation on my own. But realistically? That's a couple months of evenings and weekends reading datasheets and debugging timing issues. And honestly, it wasn't that serious to me — I wasn't going to dedicate that kind of time to it.

But because I'm proficient with agentic coding, I got the entire Dreamcast Maple Bus driver — PIO programs, DMA ring buffers, protocol implementation, the works — done in one day. Not because the AI wrote it and I watched. Because I knew how to direct it: write good context files, use memory to persist knowledge between sessions, use plans to stay on track, feed it real hardware test results, and iterate fast.

The studies on AI coding productivity show modest gains — [26% more PRs per week](https://economics.mit.edu/sites/default/files/inline-files/draft_copilot_experiments.pdf) in Microsoft's best study, [21% faster](https://arxiv.org/html/2410.12944v2) in Google's internal RCT. But those studies measure experienced developers doing work they already know how to do. The real unlock is when AI lets you tackle problems you *wouldn't have attempted otherwise* — not because you can't, but because the time investment wasn't worth it. That's where the multiplier is infinite: from zero to shipped.

Oh — and yes, even this document was written by AI. If that bothers you, I'm sorry. Show me on this doll where I hurt you.

---

## Project Links

- **This repo:** https://github.com/t3chnicallyinclined/GP2040-CE-NOBD
- **Claude Code:** https://claude.com/claude-code
- **GP2040-CE (upstream):** https://gp2040-ce.info/
- **CLAUDE.md (the context file):** [../CLAUDE.md](../CLAUDE.md)
