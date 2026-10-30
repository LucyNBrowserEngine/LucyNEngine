// test/example.js — five jobs on five websites, driven by LucyN.
//
// Run it from anywhere with Node 18+ (22.18+ can even run it straight from the
// repo, since it falls back to the local TypeScript client):
//
//     node test/example.js
//
// It opens a real window that follows the script (pass LUCYN_HEADLESS=1 for no
// window). If an engine is already running (for example `lucyn --listen` in
// another terminal) the script HOOKS INTO IT instead of starting a second one.
//
//     LUCYN_BIN=/path/to/lucyn node test/example.js    # pick a specific binary

// The package when installed (npx jsr add @lucyn/browser); the repo's own
// client source otherwise (Node >= 22.18 imports .ts via type stripping).
let Lucyn;
try {
  ({ default: Lucyn } = await import("@lucyn/browser"));
} catch {
  ({ default: Lucyn } = await import("../client/src/index.ts"));
}

const headless = Boolean(process.env.LUCYN_HEADLESS);
const browser = new Lucyn({ settleTimeoutMs: 10000, headless });

let passed = 0;
let failed = 0;

async function job(name, url, fn) {
  const t0 = Date.now();
  process.stdout.write(`\n=== ${name}\n    ${url}\n`);
  try {
    await fn();
    passed++;
    console.log(`    ok (${((Date.now() - t0) / 1000).toFixed(1)}s)`);
  } catch (err) {
    failed++;
    console.log(`    FAILED: ${err?.message ?? err}`);
  }
}

const say = (label, value) =>
  console.log(`    ${label.padEnd(14)} ${String(value).replace(/\s+/g, " ").trim().slice(0, 110)}`);

// ---- 1. example.com — load a page and pull its text ------------------------
await job("1/5  baseline: visit + extract", "https://example.com/", async () => {
  const { status, title } = await browser.visit("https://example.com/");
  say("status", status);
  say("title", title);
  if (status !== 200) throw new Error(`expected 200, got ${status}`);
  // example.com's current page carries a paragraph, not a heading.
  say("paragraph", await browser.extract("p"));
  say("links", (await browser.extractAll("a")).length);
});

// ---- 2. Amazon — scrape a real shopping results page ------------------------
await job("2/5  amazon: titles + prices", "https://www.amazon.com/s?k=python+books", async () => {
  await browser.visit("https://www.amazon.com/s?k=python+books");
  await browser.waitFor("div[data-component-type='s-search-result']", { timeout: 15000 });
  const titles = await browser.extractAll("div[data-component-type='s-search-result'] h2 span");
  const prices = await browser.extractAll("div[data-component-type='s-search-result'] .a-price .a-offscreen");
  say("results", titles.length);
  titles.slice(0, 3).forEach((t, i) => say(`book ${i + 1}`, t));
  prices.slice(0, 3).forEach((p, i) => say(`price ${i + 1}`, p));
  if (titles.length === 0) throw new Error("no results — Amazon may be showing a check page");
});

// ---- 3. Hacker News — front page headlines ---------------------------------
await job("3/5  hacker news: top stories", "https://news.ycombinator.com/", async () => {
  await browser.visit("https://news.ycombinator.com/");
  await browser.waitFor(".titleline", { timeout: 10000 });
  const titles = await browser.extractAll(".titleline > a");
  const scores = await browser.extractAll(".score");
  say("stories", titles.length);
  titles.slice(0, 3).forEach((t, i) => say(`#${i + 1}`, t));
  if (scores.length) say("top score", scores[0]);
});

// ---- 4. Wikipedia — read an article ----------------------------------------
await job("4/5  wikipedia: article intro", "https://en.wikipedia.org/wiki/Chromium_(web_browser)", async () => {
  const { status, title } = await browser.visit("https://en.wikipedia.org/wiki/Chromium_(web_browser)");
  say("status", status);
  say("title", title);
  // Wikipedia's HTML is huge — the document can still be arriving when visit()
  // settles, so wait for the heading instead of racing it.
  await browser.waitFor("#firstHeading", { timeout: 15000 });
  say("heading", await browser.extract("#firstHeading"));
  // The lead paragraphs are nested (not direct children) in Wikipedia's current markup.
  const intro = await browser.extract(".mw-parser-output p:not(.mw-empty-elt)");
  say("intro", intro.slice(0, 110) + "...");
  say("references", (await browser.extractAll(".reflist a")).length);
});

// ---- 5. quotes.toscrape.com — scrape, click, scrape again -------------------
await job("5/5  quotes: scrape + click Next", "https://quotes.toscrape.com/", async () => {
  await browser.visit("https://quotes.toscrape.com/");
  await browser.waitFor(".quote .text", { timeout: 10000 });
  const first = await browser.extractAll(".quote .text");
  say("page 1 quotes", first.length);
  say("first quote", first[0] ?? "(none)");
  // A real click that navigates, then scrape page 2.
  await browser.click("li.next > a");
  // waitIdle is only a hint here: the previous page can leave a hanging request
  // behind, which would make it time out. Poll for the content change instead.
  try {
    await browser.waitIdle({ quiet: 300, timeout: 5000 });
  } catch {
    /* the poll below decides */
  }
  let second = first;
  const deadline = Date.now() + 15000;
  while (Date.now() < deadline && (second.length === 0 || second[0] === first[0])) {
    await new Promise((r) => setTimeout(r, 400));
    second = await browser.extractAll(".quote .text");
  }
  say("page 2 quotes", second.length);
  if (second.length === 0 || second[0] === first[0]) {
    throw new Error("the click did not navigate to page 2");
  }
  say("page 2 first", second[0]);
});

// ---- summary ----------------------------------------------------------------
const { url, captures } = await browser.info();
console.log(`\n${passed}/5 jobs passed, ${failed} failed.`);
console.log(`engine ended on ${url} after capturing ${captures} requests.`);

if (headless) {
  await browser.close();     // headless runs their own engine: shut it down
} else {
  // Headful: leave the window open so you can keep clicking — the engine also
  // stays attachable for the next script. Ctrl+C closes it.
  console.log("\nwindow left open — press Ctrl+C to quit the engine.");
  await new Promise(() => {});
}
