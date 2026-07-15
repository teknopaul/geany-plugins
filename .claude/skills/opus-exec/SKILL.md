---
name: opus-exec
description: executing a plan written by opus-plan
allowed-tools: java, bash
---

Claude Opus will write phased delivery plans in `./ai-context`.

Claude Sonnet should execute, if the agent asked to execute is Opus it should stop and report this as an error.

Arguments to this skill should be the plan name and phase.
Plan may be provided as an attached document to the chat.

For this skill, executed by Claude Sonnet, each phase should be executed in sequence.
Agent should execute phases autonomously, one after the other. until the context window is 80% full.

If the context window is 80% full it should report progress, and wait for human user to clear the
context for the subsequent phases. 

During execution

Never submit code.
Never change .perf.targets - humans must review performance regressions

After each phase is complete update the _PLAM.md, so that we can recover from crashes and full context windows.
