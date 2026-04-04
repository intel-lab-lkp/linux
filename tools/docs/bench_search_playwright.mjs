#!/usr/bin/env node
// SPDX-License-Identifier: GPL-2.0

/*
 * Benchmark one docs-search variant per invocation using an external
 * Playwright installation.
 *
 * This script intentionally does not add Node dependencies to the kernel
 * tree. Load Playwright from your local tooling via --playwright-module
 * or PLAYWRIGHT_MODULE.
 *
 * Example:
 *   PLAYWRIGHT_MODULE=file:///root/bench/search-bench/node_modules/playwright/index.mjs \
 *   node tools/docs/bench_search_playwright.mjs \
 *     --variant stock \
 *     --url http://127.0.0.1:8001/ \
 *     --query kernel \
 *     --runs 5 \
 *     --scenario default \
 *     --scenario open-pages \
 *     --scenario scroll-pages \
 *     --output-dir /root/bench/results
 */

import fs from "node:fs/promises";
import path from "node:path";
import process from "node:process";
import { pathToFileURL } from "node:url";

const DEFAULT_QUERY = "kernel";
const DEFAULT_RUNS = 3;
const DEFAULT_SCENARIOS = ["default", "open-pages", "scroll-pages"];
const DEFAULT_INITIAL_RESULTS_TIMEOUT_MS = 30000;
const DEFAULT_NAVIGATION_TIMEOUT_MS = 30000;
const DEFAULT_OBSERVATION_MS = 15000;
const DEFAULT_SCROLL_DURATION_MS = 10000;
const DEFAULT_OUTPUT_SEQUENCE_WIDTH = 4;
const VALID_SCENARIOS = new Set([...DEFAULT_SCENARIOS, "recover-pages"]);
const VALID_PAGE_VARIANTS = new Set(["advanced", "stock"]);
const VALID_PAGE_SUMMARY_MODES = new Set(["limited", "unbounded"]);
const NETWORK_PROFILES = {
  none: null,
  fast3g: {
    offline: false,
    latency: 150,
    downloadThroughput: Math.round((1.6 * 1024 * 1024) / 8),
    uploadThroughput: Math.round((0.75 * 1024 * 1024) / 8),
    connectionType: "cellular3g",
  },
  slow4g: {
    offline: false,
    latency: 40,
    downloadThroughput: Math.round((4 * 1024 * 1024) / 8),
    uploadThroughput: Math.round((3 * 1024 * 1024) / 8),
    connectionType: "cellular4g",
  },
};
const CDP_DURATION_METRICS = new Map([
  ["TaskDuration", "cdpTaskDurationMs"],
  ["ScriptDuration", "cdpScriptDurationMs"],
  ["LayoutDuration", "cdpLayoutDurationMs"],
  ["RecalcStyleDuration", "cdpRecalcStyleDurationMs"],
]);
const CDP_VALUE_METRICS = new Map([
  ["Documents", "cdpDocuments"],
  ["Frames", "cdpFrames"],
  ["JSEventListeners", "cdpJSEventListeners"],
  ["JSHeapTotalSize", "cdpJsHeapTotalSizeBytes"],
  ["JSHeapUsedSize", "cdpJsHeapUsedSizeBytes"],
  ["LayoutCount", "cdpLayoutCount"],
  ["Nodes", "cdpNodes"],
  ["RecalcStyleCount", "cdpRecalcStyleCount"],
]);
const HELP = `Usage:
  node tools/docs/bench_search_playwright.mjs --variant NAME --url URL [options]

Required:
  --variant NAME               Report label for this benchmark target, e.g. stock or hard
  --url URL                    Base URL for the docs build under test

Options:
  --expected-page-variant NAME Expected page type for mismatch notes: stock, advanced
                               Default: inferred from --variant when possible
  --query TERM                 Search term to benchmark (default: ${DEFAULT_QUERY})
  --runs N                     Cold runs per scenario (default: ${DEFAULT_RUNS})
  --scenario NAME              Scenario to run; repeatable
                               Values: default, open-pages, scroll-pages, recover-pages
                               Default: ${DEFAULT_SCENARIOS.join(", ")}
  --navigation-timeout-ms N    Timeout for page.goto (default: ${DEFAULT_NAVIGATION_TIMEOUT_MS})
  --initial-results-timeout-ms N
                               Timeout waiting for first visible results (default: ${DEFAULT_INITIAL_RESULTS_TIMEOUT_MS})
  --observation-ms N           Time to keep observing after scenario actions (default: ${DEFAULT_OBSERVATION_MS})
  --scroll-duration-ms N       Scroll time for scroll-pages (default: ${DEFAULT_SCROLL_DURATION_MS})
  --network-profile NAME       Network emulation: none, fast3g, slow4g (default: none)
  --summary-delay-ms N         Artificial delay for summary page fetches (default: 0)
  --summary-fail-rate R        Artificial HTTP 503 rate for summary page fetches, 0..1 (default: 0)
  --page-summary-mode MODE     Advanced Pages summary mode: limited, unbounded
                               Default: limited
  --collect-app-timings        Collect app-side timings when the page exposes them
  --collect-cdp-metrics        Collect coarse CDP performance metrics for the page
  --playwright-module SPEC     Module specifier or file path for Playwright
  --output PATH                Write the JSON report to PATH, overwriting if it exists
  --output-dir PATH            Auto-number report files under PATH, e.g. 0001-stock-kernel.json
  --headed                     Run Chromium headed
  --help                       Show this help

Playwright loading:
  The script does not depend on Playwright being installed in this repo.
  Use --playwright-module or PLAYWRIGHT_MODULE to point at your local
  Playwright install. Examples:

    --playwright-module playwright
    --playwright-module /abs/path/to/node_modules/playwright/index.mjs
    PLAYWRIGHT_MODULE=file:///abs/path/to/node_modules/playwright/index.mjs
`;

const wait = (milliseconds) =>
  new Promise((resolve) => {
    setTimeout(resolve, milliseconds);
  });

const parseInteger = (value, flagName) => {
  const parsed = Number.parseInt(value, 10);
  if (!Number.isFinite(parsed) || parsed < 0) {
    throw new Error(`${flagName} must be a non-negative integer: ${value}`);
  }
  return parsed;
};

const parseFloatRate = (value, flagName) => {
  const parsed = Number.parseFloat(value);
  if (!Number.isFinite(parsed) || parsed < 0 || parsed > 1) {
    throw new Error(`${flagName} must be between 0 and 1: ${value}`);
  }
  return parsed;
};

const normalizeModuleSpecifier = (specifier) => {
  if (!specifier) return specifier;
  if (specifier.startsWith("file://")) return specifier;
  if (path.isAbsolute(specifier)) return pathToFileURL(specifier).href;
  if (specifier.startsWith("./") || specifier.startsWith("../")) {
    return pathToFileURL(path.resolve(specifier)).href;
  }
  return specifier;
};

const sanitizeSlugPart = (value) => {
  const normalized = String(value || "")
    .toLowerCase()
    .replace(/[^a-z0-9]+/giu, "-")
    .replace(/^-+|-+$/gu, "")
    .replace(/-{2,}/gu, "-");
  return normalized || "run";
};

const inferExpectedPageVariant = (variantName) => {
  const normalized = String(variantName || "").toLowerCase();
  if (normalized === "stock") return "stock";
  if (normalized === "hard" || normalized === "advanced") return "advanced";
  return "";
};

const requireValue = (argv, index, flagName) => {
  if (index + 1 >= argv.length) {
    throw new Error(`${flagName} requires a value`);
  }
  return argv[index + 1];
};

const parseArgs = (argv) => {
  const options = {
    collectAppTimings: false,
    collectCdpMetrics: false,
    expectedPageVariant: "",
    headed: false,
    initialResultsTimeoutMs: DEFAULT_INITIAL_RESULTS_TIMEOUT_MS,
    networkProfile: "none",
    navigationTimeoutMs: DEFAULT_NAVIGATION_TIMEOUT_MS,
    observationMs: DEFAULT_OBSERVATION_MS,
    output: "",
    outputDir: "",
    pageSummaryMode: "limited",
    playwrightModule: process.env.PLAYWRIGHT_MODULE || "",
    query: DEFAULT_QUERY,
    runs: DEFAULT_RUNS,
    scenarios: [],
    scrollDurationMs: DEFAULT_SCROLL_DURATION_MS,
    summaryDelayMs: 0,
    summaryFailRate: 0,
    url: "",
    variant: "",
  };

  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index];
    switch (argument) {
      case "--variant":
        options.variant = requireValue(argv, index, argument);
        index += 1;
        break;
      case "--url":
        options.url = requireValue(argv, index, argument);
        index += 1;
        break;
      case "--expected-page-variant":
        options.expectedPageVariant = requireValue(argv, index, argument).toLowerCase();
        index += 1;
        break;
      case "--query":
        options.query = requireValue(argv, index, argument);
        index += 1;
        break;
      case "--runs":
        options.runs = parseInteger(requireValue(argv, index, argument), argument);
        index += 1;
        break;
      case "--scenario":
        options.scenarios.push(requireValue(argv, index, argument));
        index += 1;
        break;
      case "--navigation-timeout-ms":
        options.navigationTimeoutMs = parseInteger(requireValue(argv, index, argument), argument);
        index += 1;
        break;
      case "--initial-results-timeout-ms":
        options.initialResultsTimeoutMs = parseInteger(requireValue(argv, index, argument), argument);
        index += 1;
        break;
      case "--observation-ms":
        options.observationMs = parseInteger(requireValue(argv, index, argument), argument);
        index += 1;
        break;
      case "--scroll-duration-ms":
        options.scrollDurationMs = parseInteger(requireValue(argv, index, argument), argument);
        index += 1;
        break;
      case "--network-profile":
        options.networkProfile = requireValue(argv, index, argument);
        index += 1;
        break;
      case "--summary-delay-ms":
        options.summaryDelayMs = parseInteger(requireValue(argv, index, argument), argument);
        index += 1;
        break;
      case "--summary-fail-rate":
        options.summaryFailRate = parseFloatRate(requireValue(argv, index, argument), argument);
        index += 1;
        break;
      case "--page-summary-mode":
        options.pageSummaryMode = requireValue(argv, index, argument).toLowerCase();
        index += 1;
        break;
      case "--collect-app-timings":
        options.collectAppTimings = true;
        break;
      case "--collect-cdp-metrics":
        options.collectCdpMetrics = true;
        break;
      case "--playwright-module":
        options.playwrightModule = requireValue(argv, index, argument);
        index += 1;
        break;
      case "--output":
        options.output = requireValue(argv, index, argument);
        index += 1;
        break;
      case "--output-dir":
        options.outputDir = requireValue(argv, index, argument);
        index += 1;
        break;
      case "--headed":
        options.headed = true;
        break;
      case "--help":
        options.help = true;
        break;
      default:
        throw new Error(`Unknown argument: ${argument}`);
    }
  }

  if (options.help) return options;

  if (!options.variant) throw new Error("--variant is required");
  if (!options.url) throw new Error("--url is required");
  if (options.output && options.outputDir) {
    throw new Error("--output and --output-dir are mutually exclusive");
  }
  if (!Object.prototype.hasOwnProperty.call(NETWORK_PROFILES, options.networkProfile)) {
    throw new Error(`Unsupported --network-profile: ${options.networkProfile}`);
  }
  if (options.expectedPageVariant && !VALID_PAGE_VARIANTS.has(options.expectedPageVariant)) {
    throw new Error(`Unsupported --expected-page-variant: ${options.expectedPageVariant}`);
  }
  if (!VALID_PAGE_SUMMARY_MODES.has(options.pageSummaryMode)) {
    throw new Error(`Unsupported --page-summary-mode: ${options.pageSummaryMode}`);
  }

  if (options.scenarios.length === 0) options.scenarios = DEFAULT_SCENARIOS.slice();
  options.scenarios.forEach((scenario) => {
    if (!VALID_SCENARIOS.has(scenario)) {
      throw new Error(`Unsupported --scenario: ${scenario}`);
    }
  });

  if (!options.expectedPageVariant) {
    options.expectedPageVariant = inferExpectedPageVariant(options.variant);
  }

  return options;
};

const ensureBaseUrl = (value) => {
  const url = new URL(value);
  if (!url.pathname.endsWith("/")) url.pathname += "/";
  return url;
};

const buildSearchUrl = (baseUrl, query) => {
  const url = new URL("search.html", baseUrl);
  url.searchParams.set("q", query);
  return url;
};

// Identify HTML document fetches triggered by page-summary loading while
// excluding static assets and Sphinx search infrastructure requests.
const isSummaryRequest = (requestUrl, searchUrl) => {
  let url;
  try {
    url = new URL(requestUrl);
  } catch {
    return false;
  }

  if (url.origin !== searchUrl.origin) return false;
  if (url.pathname === searchUrl.pathname) return false;
  if (url.pathname.includes("/_static/")) return false;
  if (
    url.pathname.endsWith("/searchindex.js")
    || url.pathname.endsWith("/language_data.js")
    || url.pathname.endsWith("/documentation_options.js")
  ) {
    return false;
  }
  if (/\.(?:js|css|png|svg|jpg|jpeg|gif|webp|ico|json|map|txt|woff2?|ttf)$/iu.test(url.pathname)) {
    return false;
  }

  return url.pathname.endsWith(".html") || url.pathname.endsWith("/");
};

const loadPlaywright = async (moduleSpecifier) => {
  const attempts = [];
  if (moduleSpecifier) attempts.push(normalizeModuleSpecifier(moduleSpecifier));
  else attempts.push("playwright");

  let lastError = null;
  for (const attempt of attempts) {
    try {
      const playwright = await import(attempt);
      const chromium = playwright.chromium || playwright.default?.chromium;
      if (!chromium) {
        throw new Error(`Playwright module does not export chromium: ${attempt}`);
      }
      return {
        chromium,
        resolvedSpecifier: attempt,
      };
    } catch (error) {
      lastError = error;
    }
  }

  const hint = moduleSpecifier
    ? `Failed to import Playwright from ${moduleSpecifier}`
    : "Failed to import Playwright. Set --playwright-module or PLAYWRIGHT_MODULE.";
  throw new Error(`${hint}\n${lastError}`);
};

// Inject page-side observers before app scripts run so the harness can
// capture DOM and index-readiness milestones from the page itself.
const addBenchmarkInitScript = async (page) => {
  await page.addInitScript(() => {
    const bench = {
      firstResultLinkMs: null,
      firstResolvedSummaryMs: null,
      firstSummaryStateMs: null,
      searchIndexReadyMs: null,
      searchIndexReadySource: null,
    };
    const markSearchIndexReady = (source) => {
      if (bench.searchIndexReadyMs !== null) return;
      bench.searchIndexReadyMs = performance.now();
      bench.searchIndexReadySource = source;
    };
    const getSearchGlobal = () => {
      try {
        if (typeof Search !== "undefined") return Search;
      } catch {
        // Ignore missing global lexical bindings and fall back to window.Search.
      }
      return window.Search || null;
    };
    const installSearchIndexHook = () => {
      const search = getSearchGlobal();
      if (!search || typeof search !== "object") return false;
      if (search.__kernelSearchBenchWrapped) return true;
      if (typeof search.setIndex !== "function") return false;

      const originalSetIndex = search.setIndex;
      search.setIndex = function benchmarkWrappedSetIndex(...args) {
        markSearchIndexReady("setIndex");
        return originalSetIndex.apply(this, args);
      };
      search.__kernelSearchBenchWrapped = true;
      return true;
    };
    const pollSearchIndexReady = () => {
      installSearchIndexHook();
      const search = getSearchGlobal();
      if (!search || typeof search !== "object") return false;

      const ready = (typeof search.hasIndex === "function" && search.hasIndex())
        || (typeof search._index !== "undefined" && search._index !== null);
      if (!ready) return false;

      markSearchIndexReady("hasIndex");
      return true;
    };
    const check = () => {
      if (
        bench.firstResultLinkMs === null
        && document.querySelector("#kernel-search-results .kernel-search-result a, #search-results li a")
      ) {
        bench.firstResultLinkMs = performance.now();
      }

      if (
        bench.firstSummaryStateMs === null
        && document.querySelector("#kernel-search-results .kernel-search-summary, #search-results p.context")
      ) {
        bench.firstSummaryStateMs = performance.now();
      }

      if (
        bench.firstResolvedSummaryMs === null
        && document.querySelector(
          "#kernel-search-results .kernel-search-summary:not(.kernel-search-summary-status), #search-results p.context",
        )
      ) {
        bench.firstResolvedSummaryMs = performance.now();
      }
    };

    const startObserver = () => {
      const root = document.documentElement;
      if (!root) return;
      const observer = new MutationObserver(check);
      observer.observe(root, { childList: true, subtree: true });
      check();
    };
    const searchIndexPoll = window.setInterval(() => {
      if (pollSearchIndexReady()) {
        window.clearInterval(searchIndexPoll);
      }
    }, 5);

    window.__kernelSearchBench = bench;
    if (document.readyState === "loading") {
      document.addEventListener("DOMContentLoaded", startObserver, { once: true });
    } else {
      startObserver();
    }
    window.addEventListener("load", pollSearchIndexReady, { once: true });
  });
};

const waitForInitialResults = async (page, timeoutMs) => {
  await page.waitForFunction(
    () =>
      !!document.querySelector("#kernel-search-results .kernel-search-result a, #search-results li a"),
    { timeout: timeoutMs },
  );
};

const detectPageVariant = async (page) =>
  page.evaluate(() => (document.getElementById("kernel-search-results") ? "advanced" : "stock"));

const selectPagesViewIfPresent = async (page, pageVariant, pageSummaryMode) => {
  if (pageVariant !== "advanced") {
    return {
      action: "no-op-no-pages-tab",
      effective: false,
      note: "stock quick search has no Pages tab",
      pagesTabSelected: false,
    };
  }

  const tab = page.locator("#kernel-search-tab-text");
  if ((await tab.count()) === 0) {
    return {
      action: "no-op-no-pages-tab",
      effective: false,
      note: "advanced search returned no Pages tab for this query",
      pagesTabSelected: false,
    };
  }

  const alreadySelected = await tab.evaluate(
    (element) => element.getAttribute("aria-selected") === "true",
  );
  if (!alreadySelected) {
    await tab.click();
    await page.waitForTimeout(250);
  }

  const summaryToggle = page.locator(".kernel-search-summary-limit input[type='checkbox']");
  if ((await summaryToggle.count()) > 0) {
    const shouldBeChecked = pageSummaryMode === "limited";
    const isChecked = await summaryToggle.isChecked();
    if (isChecked !== shouldBeChecked) {
      await summaryToggle.click();
      await page.waitForTimeout(100);
    }
  }

  return {
    action: alreadySelected ? "pages-tab-already-selected" : "selected-pages-tab",
    effective: true,
    note: pageSummaryMode === "unbounded"
      ? "selected Pages tab and disabled the default summary limit"
      : "selected Pages tab",
    pagesTabSelected: true,
  };
};

const scrollSearchPage = async (page, durationMs, pageVariant) => {
  const stepDelayMs = 250;
  const deadline = Date.now() + durationMs;

  if (pageVariant === "advanced") {
    const panel = page.locator("#kernel-search-panel-text:not([hidden])");
    if ((await panel.count()) > 0) {
      while (Date.now() < deadline) {
        await panel.evaluate((element) => {
          element.scrollBy(0, Math.max(400, Math.floor(element.clientHeight * 0.8)));
        });
        await page.waitForTimeout(stepDelayMs);
      }
      return;
    }
  }

  while (Date.now() < deadline) {
    await page.evaluate(() => {
      window.scrollBy(0, Math.max(400, Math.floor(window.innerHeight * 0.8)));
    });
    await page.waitForTimeout(stepDelayMs);
  }
};

const resetTrackingState = (tracking) => {
  tracking.requests = new Map();
  tracking.peakSummaryConcurrency = 0;
  tracking.inflightSummaryRequests = 0;
  tracking.wallStart = Date.now();
};

const collectDomMetrics = async (page) =>
  page.evaluate(() => {
    const bench = window.__kernelSearchBench || {};
    const advanced = !!document.getElementById("kernel-search-results");
    const resultKindCounts = advanced
      ? ["object", "title", "index", "text"].reduce((counts, kind) => {
        counts[kind] = document.querySelectorAll(
          `#kernel-search-results .kernel-search-result.kind-${kind}`,
        ).length;
        return counts;
      }, {})
      : { text: document.querySelectorAll("#search-results li").length };
    const availableResultKinds = Object.entries(resultKindCounts)
      .filter(([, count]) => typeof count === "number" && count > 0)
      .map(([kind]) => kind)
      .sort();
    const summaryToggle = advanced
      ? document.querySelector(".kernel-search-summary-limit input[type='checkbox']")
      : null;

    return {
      availableResultKinds,
      pageVariant: advanced ? "advanced" : "stock",
      firstResultLinkMs: bench.firstResultLinkMs ?? null,
      firstResolvedSummaryMs: bench.firstResolvedSummaryMs ?? null,
      firstSummaryStateMs: bench.firstSummaryStateMs ?? null,
      searchIndexReadyMs: bench.searchIndexReadyMs ?? null,
      searchIndexReadySource: typeof bench.searchIndexReadySource === "string"
        ? bench.searchIndexReadySource
        : null,
      pageSummaryMode: summaryToggle
        ? (summaryToggle.checked ? "limited" : "unbounded")
        : null,
      pagesTabActive: advanced
        ? Boolean(
          document.querySelector("#kernel-search-tab-text")?.getAttribute("aria-selected") === "true",
        )
        : null,
      renderedSummaries: advanced
        ? document.querySelectorAll(
          "#kernel-search-results .kernel-search-summary:not(.kernel-search-summary-status)",
        ).length
        : document.querySelectorAll("#search-results p.context").length,
      summaryPlaceholders: advanced
        ? document.querySelectorAll("#kernel-search-results .kernel-search-summary-status").length
        : 0,
      resultKindCounts,
      textResults: advanced
        ? document.querySelectorAll("#kernel-search-results .kernel-search-result.kind-text").length
        : null,
      totalResults: advanced
        ? document.querySelectorAll("#kernel-search-results .kernel-search-result").length
        : document.querySelectorAll("#search-results li").length,
    };
  });

const collectAppTimings = async (page) =>
  page.evaluate(() => {
    const perf = window.__kernelSearchPerf;
    if (!perf || typeof perf !== "object") return null;
    return {
      exact: typeof perf.exact === "boolean" ? perf.exact : null,
      kinds: Array.isArray(perf.kinds) ? perf.kinds : [],
      query: typeof perf.query === "string" ? perf.query : null,
      resultCounts: perf.resultCounts && typeof perf.resultCounts === "object"
        ? perf.resultCounts
        : null,
      timingsMs: perf.timingsMs && typeof perf.timingsMs === "object"
        ? perf.timingsMs
        : null,
      version: typeof perf.version === "number" ? perf.version : null,
    };
  });

const collectCdpPerformanceMetrics = async (client) => {
  const { metrics } = await client.send("Performance.getMetrics");
  const sourceMetrics = new Map(metrics.map((metric) => [metric.name, metric.value]));
  const collected = {};

  CDP_DURATION_METRICS.forEach((targetName, sourceName) => {
    if (!sourceMetrics.has(sourceName)) return;
    collected[targetName] = sourceMetrics.get(sourceName) * 1000;
  });
  CDP_VALUE_METRICS.forEach((targetName, sourceName) => {
    if (!sourceMetrics.has(sourceName)) return;
    collected[targetName] = sourceMetrics.get(sourceName);
  });

  return collected;
};

const snapshotRequests = (requests) =>
  [...requests.values()].map((request) => ({
    ...request,
    pending: request.endedAtMs === null,
  }));

const computeMetrics = ({
  domMetrics,
  peakSummaryConcurrency,
  requests,
}) => {
  const completedRequests = requests.filter((request) => !request.pending);
  const searchIndexRequests = requests.filter((request) => request.isSearchIndex);
  const completedSearchIndexRequests = searchIndexRequests.filter((request) => !request.pending);
  const summaryRequests = requests.filter((request) => request.isSummary);
  const failedSummaryRequests = summaryRequests.filter(
    (request) => request.failed || (request.status !== null && request.status >= 400),
  );
  const successfulSummaryRequests = summaryRequests.filter(
    (request) => !request.pending && !request.failed && (request.status === null || request.status < 400),
  );
  const totalBytes = completedRequests.reduce(
    (sum, request) => sum + (request.encodedDataLength || 0),
    0,
  );
  const summaryBytes = summaryRequests.reduce(
    (sum, request) => sum + (request.encodedDataLength || 0),
    0,
  );
  const searchIndexRequestStartMs = searchIndexRequests.length
    ? Math.min(...searchIndexRequests.map((request) => request.startedAtMs))
    : null;
  const searchIndexResponseEndMs = completedSearchIndexRequests.length
    ? Math.max(...completedSearchIndexRequests.map((request) => request.endedAtMs || 0))
    : null;
  const searchIndexEncodedBytes = searchIndexRequests.reduce(
    (sum, request) => sum + (request.encodedDataLength || 0),
    0,
  );

  return {
    firstResultLinkMs: domMetrics.firstResultLinkMs,
    firstResolvedSummaryMs: domMetrics.firstResolvedSummaryMs,
    firstSummaryStateMs: domMetrics.firstSummaryStateMs,
    lastCompletedRequestMs: completedRequests.length
      ? Math.max(...completedRequests.map((request) => request.endedAtMs || 0))
      : null,
    pageSummaryMode: domMetrics.pageSummaryMode,
    pagesTabActive: domMetrics.pagesTabActive,
    peakSummaryConcurrency,
    renderedSummaries: domMetrics.renderedSummaries,
    searchIndexEncodedBytes,
    // Approximate time from the last searchindex.js byte arriving to the
    // index becoming usable by the page.
    searchIndexReadyAfterResponseMs: (
      typeof domMetrics.searchIndexReadyMs === "number"
      && typeof searchIndexResponseEndMs === "number"
    )
      ? domMetrics.searchIndexReadyMs - searchIndexResponseEndMs
      : null,
    searchIndexReadyMs: domMetrics.searchIndexReadyMs,
    searchIndexRequestStartMs,
    searchIndexRequests: searchIndexRequests.length,
    searchIndexResponseEndMs,
    searchIndexTransferMs: (
      typeof searchIndexRequestStartMs === "number"
      && typeof searchIndexResponseEndMs === "number"
    )
      ? searchIndexResponseEndMs - searchIndexRequestStartMs
      : null,
    successfulSummaryRequests: successfulSummaryRequests.length,
    summaryBytes,
    summaryPlaceholders: domMetrics.summaryPlaceholders,
    summaryRequests: summaryRequests.length,
    summaryRequestsFailed: failedSummaryRequests.length,
    summaryRequestsPending: summaryRequests.filter((request) => request.pending).length,
    textResults: domMetrics.textResults,
    totalBytes,
    totalRequests: requests.length,
    totalResults: domMetrics.totalResults,
  };
};

const summarizeNumericObjectSet = (objects) => {
  const numericObjects = objects.filter((object) => object && typeof object === "object");
  if (numericObjects.length === 0) return null;

  const numericKeys = new Set();
  numericObjects.forEach((numericObject) => {
    Object.entries(numericObject).forEach(([key, value]) => {
      if (typeof value === "number" && Number.isFinite(value)) numericKeys.add(key);
    });
  });

  const median = {};
  const minimum = {};
  const maximum = {};

  [...numericKeys].sort().forEach((key) => {
    const values = numericObjects
      .map((numericObject) => numericObject[key])
      .filter((value) => typeof value === "number" && Number.isFinite(value))
      .sort((left, right) => left - right);
    if (values.length === 0) return;
    const middle = Math.floor(values.length / 2);
    median[key] = values.length % 2 === 0
      ? (values[middle - 1] + values[middle]) / 2
      : values[middle];
    minimum[key] = values[0];
    maximum[key] = values[values.length - 1];
  });

  return {
    maximum,
    median,
    minimum,
    sampleCount: numericObjects.length,
  };
};

const summarizeRuns = (runs) => {
  const metricSummary = summarizeNumericObjectSet(runs.map((run) => run.metrics));
  const appTimingSummary = summarizeNumericObjectSet(
    runs.map((run) => run.appTimings?.timingsMs),
  );
  const resultKindCountSummary = summarizeNumericObjectSet(
    runs.map((run) => run.domMetrics?.resultKindCounts),
  );
  const appResultCountSummary = summarizeNumericObjectSet(
    runs.map((run) => run.appTimings?.resultCounts),
  );
  const cdpMetricSummary = summarizeNumericObjectSet(
    runs.map((run) => run.cdpMetrics),
  );
  const recoveryFailureMetricSummary = summarizeNumericObjectSet(
    runs.map((run) => run.recovery?.failurePhase?.metrics),
  );
  const recoveryRecoveryMetricSummary = summarizeNumericObjectSet(
    runs.map((run) => run.recovery?.recoveryPhase?.metrics),
  );

  const firstRun = [...runs]
    .sort((left, right) => left.run - right.run)[0] || null;
  const scenarioNoOpCount = runs.filter((run) => run.scenarioNoOp).length;
  const actions = [...new Set(runs.flatMap((run) => run.actions || []))].sort();
  const availableResultKinds = [...new Set(
    runs.flatMap((run) => run.domMetrics?.availableResultKinds || []),
  )].sort();

  return {
    actions,
    allRunsNoOp: runs.length > 0 && scenarioNoOpCount === runs.length,
    availableResultKinds,
    firstRun: firstRun
      ? {
        actions: firstRun.actions || [],
        appTimings: firstRun.appTimings || null,
        cdpMetrics: firstRun.cdpMetrics || null,
        domMetrics: firstRun.domMetrics || null,
        metrics: firstRun.metrics,
        notes: firstRun.notes,
        recovery: firstRun.recovery || null,
        run: firstRun.run,
        scenarioNoOp: Boolean(firstRun.scenarioNoOp),
        variantDetected: firstRun.variantDetected,
      }
      : null,
    appResultCountSummary,
    appTimingSummary,
    cdpMetricSummary,
    maximum: metricSummary ? metricSummary.maximum : {},
    median: metricSummary ? metricSummary.median : {},
    minimum: metricSummary ? metricSummary.minimum : {},
    recoveryFailureMetricSummary,
    recoveryRecoveryMetricSummary,
    resultKindCountSummary,
    scenarioNoOpCount,
    runCount: runs.length,
  };
};

const buildSummary = (results) => {
  const scenarios = {};
  results.forEach((result) => {
    if (!scenarios[result.scenario]) scenarios[result.scenario] = {};
    if (!scenarios[result.scenario][result.variant]) scenarios[result.scenario][result.variant] = [];
    scenarios[result.scenario][result.variant].push(result);
  });

  const summary = {};
  Object.entries(scenarios).forEach(([scenario, variants]) => {
    summary[scenario] = {};
    Object.entries(variants).forEach(([variant, runs]) => {
      summary[scenario][variant] = summarizeRuns(runs);
    });

    if (summary[scenario].stock && summary[scenario].hard) {
      const delta = {};
      const stockMedian = summary[scenario].stock.median;
      const hardMedian = summary[scenario].hard.median;
      const keys = new Set([...Object.keys(stockMedian), ...Object.keys(hardMedian)]);
      [...keys].sort().forEach((key) => {
        if (typeof stockMedian[key] !== "number" || typeof hardMedian[key] !== "number") return;
        delta[key] = hardMedian[key] - stockMedian[key];
      });
      summary[scenario].deltaHardMinusStock = delta;
    }
  });

  return summary;
};

const buildAutoOutputPath = async (outputDirectory, options) => {
  await fs.mkdir(outputDirectory, { recursive: true });
  const entries = await fs.readdir(outputDirectory, { withFileTypes: true });
  let maxSequence = 0;

  entries.forEach((entry) => {
    if (!entry.isFile()) return;
    const match = entry.name.match(/^(\d+)-/u);
    if (!match) return;
    const parsed = Number.parseInt(match[1], 10);
    if (Number.isFinite(parsed)) {
      maxSequence = Math.max(maxSequence, parsed);
    }
  });

  const parts = [
    sanitizeSlugPart(options.variant),
    sanitizeSlugPart(options.query),
  ];
  if (options.networkProfile !== "none") parts.push(options.networkProfile);
  if (options.summaryDelayMs > 0) parts.push(`delay-${options.summaryDelayMs}ms`);
  if (options.summaryFailRate > 0) parts.push(`fail-${Math.round(options.summaryFailRate * 100)}pct`);
  if (options.pageSummaryMode !== "limited") parts.push(`pages-${options.pageSummaryMode}`);

  const sequence = String(maxSequence + 1).padStart(DEFAULT_OUTPUT_SEQUENCE_WIDTH, "0");
  return path.join(outputDirectory, `${sequence}-${parts.join("-")}.json`);
};

const runScenario = async ({
  browser,
  options,
  scenario,
  variant,
}) => {
  const baseUrl = ensureBaseUrl(variant.url);
  const searchUrl = buildSearchUrl(baseUrl, options.query);
  const context = await browser.newContext({
    viewport: { width: 1440, height: 900 },
  });
  const page = await context.newPage();
  await addBenchmarkInitScript(page);

  const client = await context.newCDPSession(page);
  await client.send("Network.enable");
  await client.send("Network.setCacheDisabled", { cacheDisabled: true });
  if (options.collectCdpMetrics) {
    await client.send("Performance.enable");
  }

  const networkProfile = NETWORK_PROFILES[options.networkProfile];
  if (networkProfile) {
    await client.send("Network.emulateNetworkConditions", networkProfile);
  }

  const tracking = {
    inflightSummaryRequests: 0,
    peakSummaryConcurrency: 0,
    requests: new Map(),
    wallStart: Date.now(),
  };
  const injection = {
    delayMs: options.summaryDelayMs,
    failRate: options.summaryFailRate,
  };

  client.on("Network.requestWillBeSent", (event) => {
    const request = {
      encodedDataLength: 0,
      endedAtMs: null,
      failed: false,
      isSearchIndex: event.request.url.endsWith("/searchindex.js"),
      isSummary: isSummaryRequest(event.request.url, searchUrl),
      method: event.request.method,
      requestId: event.requestId,
      resourceType: event.type || "Other",
      startedAtMs: Date.now() - tracking.wallStart,
      status: null,
      url: event.request.url,
    };
    tracking.requests.set(event.requestId, request);
    if (request.isSummary) {
      tracking.inflightSummaryRequests += 1;
      tracking.peakSummaryConcurrency = Math.max(
        tracking.peakSummaryConcurrency,
        tracking.inflightSummaryRequests,
      );
    }
  });

  client.on("Network.responseReceived", (event) => {
    const request = tracking.requests.get(event.requestId);
    if (!request) return;
    request.status = event.response.status;
  });

  const finishRequest = (event, extra = {}) => {
    const request = tracking.requests.get(event.requestId);
    if (!request) return;
    request.encodedDataLength = extra.encodedDataLength || request.encodedDataLength || 0;
    request.endedAtMs = Date.now() - tracking.wallStart;
    if (extra.errorText) request.errorText = extra.errorText;
    if (extra.failed) request.failed = true;
    if (request.isSummary) {
      tracking.inflightSummaryRequests = Math.max(0, tracking.inflightSummaryRequests - 1);
    }
  };

  client.on("Network.loadingFinished", (event) => {
    finishRequest(event, { encodedDataLength: event.encodedDataLength || 0 });
  });

  client.on("Network.loadingFailed", (event) => {
    finishRequest(event, {
      errorText: event.errorText || null,
      failed: true,
    });
  });

  if (options.summaryDelayMs > 0 || options.summaryFailRate > 0) {
    await page.route("**/*", async (route) => {
      const requestUrl = route.request().url();
      if (!isSummaryRequest(requestUrl, searchUrl)) {
        await route.continue();
        return;
      }

      if (injection.delayMs > 0) {
        await wait(injection.delayMs);
      }

      if (injection.failRate > 0 && Math.random() < injection.failRate) {
        await route.fulfill({
          body: "benchmark injected failure\n",
          contentType: "text/plain",
          status: 503,
        });
        return;
      }

      await route.continue();
    });
  }

  const notes = [];
  const actions = [];
  let scenarioNoOp = false;

  const capturePhase = async (pageVariant, collectCdp, phaseNotes) => {
    let appTimings = null;
    if (options.collectAppTimings) {
      appTimings = await collectAppTimings(page);
      if (!appTimings && pageVariant === "advanced") {
        phaseNotes.push("app timings unavailable");
      }
    }

    let cdpMetrics = null;
    if (collectCdp && options.collectCdpMetrics) {
      cdpMetrics = await collectCdpPerformanceMetrics(client);
    }

    const domMetrics = await collectDomMetrics(page);
    const requestSnapshot = snapshotRequests(tracking.requests);
    return {
      appTimings,
      cdpMetrics,
      domMetrics,
      metrics: computeMetrics({
        domMetrics,
        peakSummaryConcurrency: tracking.peakSummaryConcurrency,
        requests: requestSnapshot,
      }),
      requestCount: requestSnapshot.length,
      requests: requestSnapshot,
      variant: pageVariant,
    };
  };

  const runObservedNavigation = async ({
    enableScroll = false,
    openPages = false,
    collectCdp = true,
    phaseLabel = "",
  } = {}) => {
    resetTrackingState(tracking);
    await page.goto(searchUrl.href, {
      timeout: options.navigationTimeoutMs,
      waitUntil: "domcontentloaded",
    });
    await waitForInitialResults(page, options.initialResultsTimeoutMs);

    const pageVariant = await detectPageVariant(page);
    const phaseNotes = [];
    const phaseActions = [];
    let phaseNoOp = false;

    if (variant.expectedPageVariant && pageVariant !== variant.expectedPageVariant) {
      phaseNotes.push(`expected ${variant.expectedPageVariant}, got ${pageVariant}`);
    }

    if (openPages) {
      const action = await selectPagesViewIfPresent(page, pageVariant, options.pageSummaryMode);
      phaseActions.push(action.action);
      phaseNotes.push(action.note);
      if (!action.effective) phaseNoOp = true;
    }

    if (enableScroll) {
      await scrollSearchPage(page, options.scrollDurationMs, pageVariant);
      phaseActions.push("scrolled-page");
      phaseNotes.push(`scrolled for ${options.scrollDurationMs}ms`);
    }

    await page.waitForTimeout(options.observationMs);
    const phase = await capturePhase(pageVariant, collectCdp, phaseNotes);
    if (phaseLabel) {
      phase.phase = phaseLabel;
    }
    phase.actions = phaseActions;
    phase.notes = phaseNotes;
    phase.scenarioNoOp = phaseNoOp;
    return phase;
  };

  try {
    if (scenario === "recover-pages") {
      if (options.summaryFailRate <= 0) {
        notes.push("recover-pages is most useful with --summary-fail-rate > 0");
      }

      // Exercise fresh-navigation recovery: first load with injected summary
      // failures, then reload the same query cleanly to verify recovery behavior.
      const failurePhase = await runObservedNavigation({
        openPages: true,
        collectCdp: false,
        phaseLabel: "failure",
      });
      actions.push(...failurePhase.actions.map((action) => `failure:${action}`));
      notes.push(...failurePhase.notes.map((note) => `failure: ${note}`));

      injection.delayMs = 0;
      injection.failRate = 0;
      actions.push("reloaded-page-clean");
      notes.push("reloaded same query with summary failure injection disabled");

      const recoveryPhase = await runObservedNavigation({
        openPages: true,
        collectCdp: options.collectCdpMetrics,
        phaseLabel: "recovery",
      });
      actions.push(...recoveryPhase.actions.map((action) => `recovery:${action}`));
      notes.push(...recoveryPhase.notes.map((note) => `recovery: ${note}`));

      scenarioNoOp = Boolean(failurePhase.scenarioNoOp && recoveryPhase.scenarioNoOp);
      return {
        actions,
        appTimings: recoveryPhase.appTimings,
        cdpMetrics: recoveryPhase.cdpMetrics,
        domMetrics: recoveryPhase.domMetrics,
        metrics: recoveryPhase.metrics,
        notes,
        recovery: {
          failurePhase: {
            actions: failurePhase.actions,
            appTimings: failurePhase.appTimings,
            cdpMetrics: failurePhase.cdpMetrics,
            domMetrics: failurePhase.domMetrics,
            metrics: failurePhase.metrics,
            notes: failurePhase.notes,
            requestCount: failurePhase.requestCount,
            variantDetected: failurePhase.variant,
          },
          recoveryPhase: {
            actions: recoveryPhase.actions,
            appTimings: recoveryPhase.appTimings,
            cdpMetrics: recoveryPhase.cdpMetrics,
            domMetrics: recoveryPhase.domMetrics,
            metrics: recoveryPhase.metrics,
            notes: recoveryPhase.notes,
            requestCount: recoveryPhase.requestCount,
            variantDetected: recoveryPhase.variant,
          },
        },
        requestCount: recoveryPhase.requestCount,
        scenarioNoOp,
        variant: recoveryPhase.variant,
      };
    }

    const phase = await runObservedNavigation({
      enableScroll: scenario === "scroll-pages",
      openPages: scenario === "open-pages" || scenario === "scroll-pages",
      collectCdp: options.collectCdpMetrics,
    });
    actions.push(...phase.actions);
    notes.push(...phase.notes);
    scenarioNoOp = phase.scenarioNoOp;

    return {
      actions,
      appTimings: phase.appTimings,
      cdpMetrics: phase.cdpMetrics,
      domMetrics: phase.domMetrics,
      metrics: phase.metrics,
      notes,
      requestCount: phase.requestCount,
      scenarioNoOp,
      variant: phase.variant,
    };
  } finally {
    await context.close();
  }
};

const main = async () => {
  const options = parseArgs(process.argv.slice(2));
  if (options.help) {
    process.stdout.write(HELP);
    return;
  }

  const { chromium, resolvedSpecifier } = await loadPlaywright(options.playwrightModule);
  const browser = await chromium.launch({ headless: !options.headed });

  try {
    const variant = {
      expectedPageVariant: options.expectedPageVariant,
      name: options.variant,
      url: options.url,
    };

    const results = [];
    for (const scenario of options.scenarios) {
      for (let runIndex = 0; runIndex < options.runs; runIndex += 1) {
        console.error(
          `[bench] scenario=${scenario} variant=${variant.name} run=${runIndex + 1}/${options.runs}`,
        );
        const run = await runScenario({
          browser,
          options,
          scenario,
          variant,
        });
        results.push({
          actions: run.actions,
          appTimings: run.appTimings,
        cdpMetrics: run.cdpMetrics,
          domMetrics: run.domMetrics,
          metrics: run.metrics,
          notes: run.notes,
          recovery: run.recovery || null,
          run: runIndex + 1,
          scenario,
          scenarioNoOp: run.scenarioNoOp,
          url: variant.url,
          variant: variant.name,
          variantDetected: run.variant,
        });
      }
    }

    const report = {
      generatedAt: new Date().toISOString(),
      options: {
        collectAppTimings: options.collectAppTimings,
        collectCdpMetrics: options.collectCdpMetrics,
        expectedPageVariant: options.expectedPageVariant || null,
        initialResultsTimeoutMs: options.initialResultsTimeoutMs,
        networkProfile: options.networkProfile,
        navigationTimeoutMs: options.navigationTimeoutMs,
        observationMs: options.observationMs,
        playwrightModule: resolvedSpecifier,
        query: options.query,
        runs: options.runs,
        scenarios: options.scenarios,
        scrollDurationMs: options.scrollDurationMs,
        summaryDelayMs: options.summaryDelayMs,
        summaryFailRate: options.summaryFailRate,
        url: options.url,
        variant: options.variant,
      },
      results,
      summary: buildSummary(results),
    };

    const serialized = `${JSON.stringify(report, null, 2)}\n`;
    const outputPath = options.outputDir
      ? await buildAutoOutputPath(options.outputDir, options)
      : options.output;

    if (outputPath) {
      await fs.mkdir(path.dirname(outputPath), { recursive: true });
      await fs.writeFile(outputPath, serialized, "utf-8");
      console.error(`[bench] wrote ${outputPath}`);
    } else {
      process.stdout.write(serialized);
    }
  } finally {
    await browser.close();
  }
};

main().catch((error) => {
  console.error(error.message || error);
  process.exit(1);
});
