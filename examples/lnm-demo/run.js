import { createInterface } from "node:readline";
import Lucyn from "../../client/src/index.js";

const [action, ...args] = process.argv.slice(2);
const browser = new Lucyn({
  config: "examples/lnm-demo/engine.json",
  show: true,
});

try {
  let page;
  if (action === "search") {
    const knownEngines = new Set([
      "brave", "searxng", "google", "duckduckgo", "bing", "ecosia",
      "startpage", "mojeek", "qwant", "swisscows",
    ]);
    const engine = knownEngines.has(args[0]?.toLowerCase()) ? args.shift().toLowerCase() : undefined;
    const query = args.join(" ").trim();
    if (!query) throw new Error('Usage: node examples/lnm-demo/run.js search [engine] "search terms"');
    page = await browser.search(query, engine ? { engine } : {});
  } else if (action === "open") {
    const url = args[0];
    if (!url) throw new Error("Usage: node examples/lnm-demo/run.js open <url>");
    page = await browser.open(url);
  } else {
    throw new Error("Choose an action: search [engine] <query> | open <url>");
  }

  console.log(`Status: ${page.status}`);
  console.log(`Title:  ${page.title || "(no title)"}`);
  console.log(`URL:    ${page.url}`);
  console.log("Press Enter here to close the browser window.");
  const terminal = createInterface({ input: process.stdin, output: process.stdout });
  await new Promise((resolve) => terminal.once("line", resolve));
  terminal.close();
} finally {
  await browser.close();
}
