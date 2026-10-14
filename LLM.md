# AI-Assisted Contributions

## Does LucyN allow AI-generated code?

Yes. You may use an AI tool to explore the codebase, propose a design, write code, or help debug it. AI assistance does not make a contribution unacceptable by itself. The contributor is responsible for understanding the change, checking its behavior, and answering review questions.

## What makes an AI-assisted PR acceptable?

Explain **why the change is needed**. A PR should start with a real bug, user need, or concrete limitation in LucyN. Describe what you were trying to do, what failed, how you traced it to this part of the engine, and how the change addresses the cause. Include a reproducer or other evidence when possible, and list the build and tests you ran.

The explanation matters more than whether a person or an AI typed the code. A technically valid change with no user problem or reason to belong in LucyN may be closed without review. Small changes are welcome when they solve a real problem; size alone is not a reason to reject a PR.

## Example

> While using the JS client with an adblock rule, I got an error because the module exposed `f.string` where the caller needed `f.array`. I traced the failure from the client call through `modules/adblock` and found the value was represented with the wrong shape at the module boundary. I changed it to an array, added a reproducer for the failing request, and verified the adblock and client paths. I used an AI assistant to help investigate and draft the change, then reviewed the implementation and results myself.

This gives reviewers the user goal, the observed failure, the investigation, the module-level cause, and evidence for the fix. Replace the example with your actual experience; do not claim tests or investigation you did not perform.

## Before opening a PR

- Identify the owning module and keep the change within its public boundary where practical.
- Read that module's README and nearby implementation before proposing a redesign.
- Build the affected target and run relevant tests when available. State clearly when you could not.
- Review AI-generated output for incorrect assumptions, unrelated edits, copied code, secrets, and unsupported claims.
- Explain cross-module changes and why the module boundary needs to be crossed.

See [CONTRIBUTING.md](CONTRIBUTING.md) for the repository workflow and PR checklist.
