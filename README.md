# LP-0008 — Autonomous AI Module for Logos Core

An agent that is a first-class participant in the Logos stack rather than a
chatbot with an API key: it holds its own shielded LEZ account, stores and
retrieves files through Logos Storage, talks to its owner and to other agents
over encrypted Logos Messaging, and spends within limits its owner sets.

Agent-to-agent coordination is **A2A-compatible** — Agent Cards follow the A2A
schema and tasks follow the A2A lifecycle — with Logos Messaging as the transport
A2A leaves open, and LEZ transfers as the payment layer A2A deliberately omits.

> **Status: in progress.** This repository is being built for
> [λPrize LP-0008](https://github.com/logos-co/lambda-prize/blob/master/prizes/LP-0008.md).
> Nothing here claims to be finished, and no evidence is presented that has not
> been reproduced. See [`docs/recon.md`](docs/recon.md) for what the stack
> actually provides and where the difficulty sits.

## What has to be true for this to count

The prize's evidence bar is higher than its feature list, and five earlier
submissions were closed on evidence rather than on design. So these come first:

- **Three agents live on the LEZ public testnet**, one per skill category, each
  deployed reproducibly with the transactions to show for it.
- **Two agents discovering each other, running an A2A task, and paying each other
  in LEZ** — autonomously, with the on-chain transfer visible to anyone.
- **A demo that runs from a clean clone** with `RISC0_DEV_MODE=0`, and CI that
  actually executes an end-to-end run against a real sequencer rather than
  skipping it.
- **A recorded walkthrough against the public testnet**, not a localnet.

## Layout

```
docs/recon.md     what the Logos module contract is, and why the previous
                  five submissions were closed
```

More lands here as it is built and verified. The repository is deliberately
empty of claims until each one has evidence behind it.

## License

Dual-licensed under [MIT](LICENSE-MIT) or [Apache-2.0](LICENSE-APACHE), at your
option.
