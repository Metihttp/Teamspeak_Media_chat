# Polls

[← Back to README](../README.md)

TS Media chat 2.3 adds anonymous polls, like Telegram's. Click the paperclip at the left of the chat input, choose **Poll**, type a question (up to 200 characters) and 2 to 10 options (up to 100 characters each), and decide whether people may choose more than one option. The poll goes to your channel.

- **Everyone can read it.** A poll is an ordinary chat message:

  ```
  📊 Poll: Where do we eat on Friday?
  1. Pizza
  2. Sushi
  3. Tacos
  Vote with TS Media chat
  ```

  "Vote with TS Media chat" links to the plugin's page. People without the plugin read the poll as text.
- **People with the plugin see a poll card.** It shows the question, "Anonymous poll" and the options. Click an option to vote. In a poll with multiple answers, tick options and click **Vote**. After you vote you see each option's share in percent, its number of votes and a bar. Your choice has a check mark. The card also shows the number of votes and a **Retract vote** button. After retracting you can vote again.
- **Votes are taken in the channel the poll was posted in.** Elsewhere, the card says so.

## Anonymity

Nobody sees who voted for what. That includes the person who made the poll and everyone else who voted. TS Media never shows a voter, and it never stores anything that names one.

How it works:

- A vote is a **ballot**, not a person. When you first vote on a poll, your plugin makes a random secret for that poll and a chain of hashes from it. The ballot's id is the last link of the chain.
- Every change of your ballot reveals the next link back: a vote, a different choice, or a retraction (no options). Anyone can check that a new state belongs to the ballot by hashing it forward. Only you can make a later state, because only you have the secret.
  - Nobody else can take your ballot further: change it to a newer state or retract it.
  - The newest state wins everywhere.
  - Nothing in a ballot says whose it is.
- Every plugin user in the channel stores the ballots they have seen in `polls.json`, in TS Media's data folder. When a poll is shown to someone who joined later, their plugin asks the channel what it has. Someone who has more answers with the ballots. The answer comes from whoever answers, not from the voters. The counts therefore survive the voters leaving or going offline.
- Your secret stays in your own `polls.json`, which is written as soon as you vote. It lets your plugin change or retract your vote later, also after a restart. The polls you voted on are the last ones `polls.json` lets go of when it is full.
- When TS Media receives a plugin command, it checks only that the sender is in your channel right now. Then it drops the sender's identity. Nothing about the sender is kept.
- TS Media only takes ballots for polls that one of your chats shows, posted in the channel you are in. Ballots for anything else are ignored, so nobody can fill your `polls.json` with made-up polls.

### Honest limits

- **The server tells every receiver who sent each plugin command.** TS Media ignores that information. A modified client could log it, though, and link a live vote to the person who cast it at that moment. Ballots passed on to late joiners usually don't carry that link, because they come from whoever answers. There is one exception: when only the voter still has their ballot (nobody else with the plugin was there to store it), the voter's own plugin is the only one that can answer with it.
- **A modified client can invent ballots.** The counts are only as trustworthy as the people with the plugin in the channel. TS Media rejects malformed ballots, forged changes and impossible answers, such as two options in a one-answer poll. It also limits each poll to 1000 ballots and each person to 40 plugin commands in 30 seconds.
- **The proof doesn't cover the options.** A modified client that passes a ballot on can show its newest state with other options to people who haven't received that state yet. They keep seeing it that way until the voter changes their vote again. This is no worse than inventing ballots, which a modified client can do anyway.
- **A copy of a poll message shares the poll's counts.** The poll's id is in the message, so a reposted copy is the same poll. Someone could also post a different text under the same id. Its card would then show that poll's counts under other labels. The card always shows the message's own sender.
- **A server that doesn't pass plugin commands on.** On such a server, polls can still be posted and read, but votes can't be sent. The card says so.

## Protocol

Polls use TS Media's plugin commands, protocol `tsm1` (`src/peerprotocol.h`). Two message types are added; older versions ignore them.

| message | meaning |
|---|---|
| `PV q=<poll> v=<entry>,... [y=1]` | ballot states: up to 8 per command. `y=1` marks ballots passed on to a late joiner. |
| `PQ q=<poll>:<count>:<digest>,...` | "what do you have?", with what the asker has: up to 8 polls per command. |

- An entry has the form `<id>.<step>.<proof>.<options>`:
  - `id` and `proof` are 16 bytes each, as base64url without padding.
  - `step` runs from 1 to 120.
  - `options` is a hex bit mask: bit *i* stands for option *i* + 1, and 0 means retracted.
- A state is accepted only when hashing `proof` forward `step − known step` times gives the state already known: the ballot's id for a new ballot, otherwise the last proof.
- The hash is the first 16 bytes of SHA-256 over `"tsmpoll1|" + poll + "|" + value`.
- A poll is identified by a 16-digit hex id. The marker link's fragment carries it: `#tsmpoll1-<id>-<n><s|m>-<channel id>`, where `n` is the number of options, `s` means one answer and `m` means multiple answers.

### Timing and flood control

- **Your vote** is sent at once, with the priority of reactions. A newer state replaces one still waiting in the queue.
  - If it can't be sent, it goes again after 2 s and after 5 s. Each retry is a new state of the ballot.
  - After that, the card says your vote didn't reach the others and offers **Retry**.
- **The question about a poll** (`PQ`) goes 1.5 s after the poll appears in your channel's chat. It is asked at most every 2 minutes per poll, and only when someone in the channel has TS Media.
- **Answers** come 0.3–1.5 s later, at background priority and with at most 64 ballots per answer. They are sent at most every 20 s per poll, and at most 16 answer commands per 20 s per server, whoever asks about whichever polls. An answer overheard first takes those ballots off your own answer. One answer therefore usually serves everyone. Questions and answers only go to the channel the poll was posted in, and only while you are in it.
- **With presence turned off** (**Settings → Privacy & updates → Tell people in your channel that you have TS Media**), your plugin neither asks nor answers. Only the votes you cast go out, so you may not see votes cast before you joined.
- All plugin commands spend the connection's plugin flood bucket (`FloodGovernor`).

### Limits

| what | limit |
|---|---|
| polls in `polls.json` | 300; the polls inactive longest go first, the ones you voted on last |
| ballots per poll | 1000 |
| ballots in `polls.json` | 60,000 in all |
| states per ballot | 120 |
| message size | the whole poll fits in one chat message, which the New poll window checks |
