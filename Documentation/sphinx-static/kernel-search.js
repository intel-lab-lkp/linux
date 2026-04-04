"use strict";

// Client-side search UI for Documentation/search.html.
//
// This reuses Sphinx-generated language_data.js and searchindex.js,
// groups results by kind, applies URL-driven filters, and lazily fetches
// page text for "Pages" summaries.
(() => {
  const RESULT_KIND_ORDER = ["object", "title", "index", "text"];
  const RESULT_KIND_LABELS = {
    object: "Symbols",
    title: "Sections",
    index: "Index entries",
    text: "Pages",
  };
  const TOP_LEVEL_AREA = "__top_level__";
  // Search ranking policy: higher scores sort first.
  const LABEL_MATCH_SCORES = {
    exactMatchBonus: 20,
    exactCandidateBonus: 10,
    sectionBase: 15,
    sectionPartial: 7,
    indexBase: 20,
    indexPartial: 8,
    secondaryIndexPenalty: 5,
  };
  const OBJECT_MATCH_SCORES = {
    exact: 120,
    exactNameBoost: 11,
    partialShortName: 6,
    partialFullName: 4,
    matchedNameTerm: 1,
  };
  const TEXT_MATCH_SCORES = {
    term: 5,
    partialTerm: 2,
    titleTerm: 15,
    partialTitleTerm: 7,
    exactTitleBonus: 10,
  };
  // Sphinx object priorities: 0 = important, 1 = default, 2 = unimportant.
  const OBJECT_PRIORITY = {
    0: 15,
    1: 5,
    2: -5,
  };
  const SUMMARY_FETCH_BUDGET = 50;
  const SUMMARY_RESULT_LIMIT = 50;
  const SUMMARY_VIEWPORT_MARGIN = "200px 0px";
  const documentTextCache = new Map();
  let summaryGeneration = 0;
  let summaryQueue = [];
  let summaryPayloads = [];
  let summaryViewportObserver = null;
  let summaryViewportRoot = null;
  let activeFetchCount = 0;
  let activeResultKind = RESULT_KIND_ORDER[0];
  let pageSummaryLimitEnabled = true;
  let tabStripCleanup = null;

  // Hook into Sphinx's asynchronous searchindex.js loading.
  window.Search = window.Search || {};
  window.Search._callbacks = window.Search._callbacks || [];
  window.Search._index = window.Search._index || null;
  window.Search.setIndex = (index) => {
    window.Search._index = index;
    const callbacks = window.Search._callbacks.slice();
    window.Search._callbacks.length = 0;
    callbacks.forEach((callback) => callback(index));
  };
  window.Search.whenReady = (callback) => {
    if (window.Search._index) callback(window.Search._index);
    else window.Search._callbacks.push(callback);
  };

  // Query normalization and Sphinx compatibility helpers.
  const splitQuery = (query) =>
    query
      .split(/[^\p{Letter}\p{Number}_\p{Emoji_Presentation}]+/gu)
      .filter((term) => term);

  // Fall back to an identity stemmer so search still works if a future
  // Sphinx change stops providing the Stemmer global.
  const getStemmer = () =>
    typeof Stemmer === "function" ? new Stemmer() : { stemWord: (word) => word };

  // Sphinx <= 8 exposes stopwords as an array; 9.x switched to a Set.
  const hasStopword = (word) => {
    if (typeof stopwords === "undefined") return false;
    if (typeof stopwords.has === "function") return stopwords.has(word);
    if (typeof stopwords.indexOf === "function") return stopwords.indexOf(word) !== -1;
    return false;
  };

  const hasOwn = (object, key) =>
    Object.prototype.hasOwnProperty.call(object, key);

  // Prefer the newer content-root data attribute, but fall back to older
  // Sphinx builds that still expose URL_ROOT on DOCUMENTATION_OPTIONS.
  const getContentRoot = () =>
    document.documentElement.dataset.content_root
    || (typeof DOCUMENTATION_OPTIONS !== "undefined" ? DOCUMENTATION_OPTIONS.URL_ROOT || "" : "");

  // General utilities, result ordering, and generated-document paths.
  const compareResults = (left, right) => {
    if (left.score === right.score) {
      const leftTitle = left.title.toLowerCase();
      const rightTitle = right.title.toLowerCase();
      if (leftTitle === rightTitle) return 0;
      return leftTitle < rightTitle ? -1 : 1;
    }
    return right.score - left.score;
  };

  const getAreaValue = (docName) =>
    docName.includes("/") ? docName.split("/", 1)[0] : TOP_LEVEL_AREA;

  const getAreaLabel = (area) =>
    area === TOP_LEVEL_AREA ? "Top level" : area;

  const matchArea = (docName, area) => {
    if (!area) return true;
    if (area === TOP_LEVEL_AREA) return !docName.includes("/");
    return docName === area || docName.startsWith(area + "/");
  };

  // Generated-document path handling for html and dirhtml builds.
  const buildDocUrls = (docName) => {
    const contentRoot = getContentRoot();
    const builder = DOCUMENTATION_OPTIONS.BUILDER;
    const fileSuffix = DOCUMENTATION_OPTIONS.FILE_SUFFIX;
    const linkSuffix = DOCUMENTATION_OPTIONS.LINK_SUFFIX;

    if (builder === "dirhtml") {
      let dirname = docName + "/";
      if (dirname.match(/\/index\/$/)) dirname = dirname.substring(0, dirname.length - 6);
      else if (dirname === "index/") dirname = "";

      return {
        requestUrl: contentRoot + dirname,
        linkUrl: contentRoot + dirname,
      };
    }

    return {
      requestUrl: contentRoot + docName + fileSuffix,
      linkUrl: docName + linkSuffix,
    };
  };

  // Lazy page-summary helpers for "Pages" search results.
  const htmlToText = (htmlString, anchor) => {
    const htmlElement = new DOMParser().parseFromString(htmlString, "text/html");
    for (const selector of [".headerlink", "script", "style"]) {
      htmlElement.querySelectorAll(selector).forEach((element) => element.remove());
    }

    if (anchor) {
      const anchorId = anchor[0] === "#" ? anchor.substring(1) : anchor;
      const anchorContent = htmlElement.getElementById(anchorId);
      if (anchorContent) return anchorContent.textContent;
    }

    const docContent = htmlElement.querySelector('[role="main"]');
    return docContent ? docContent.textContent : "";
  };

  const makeSummary = (htmlText, keywords, anchor) => {
    const text = htmlToText(htmlText, anchor);
    if (!text) return null;

    const lowered = text.toLowerCase();
    const positions = keywords
      .map((keyword) => lowered.indexOf(keyword.toLowerCase()))
      .filter((position) => position > -1);
    const actualStart = positions.length ? positions[0] : 0;
    const start = Math.max(actualStart - 120, 0);
    const prefix = start === 0 ? "" : "...";
    const suffix = start + 240 < text.length ? "..." : "";

    const summary = document.createElement("p");
    summary.className = "kernel-search-summary";
    summary.textContent = prefix + text.substring(start, start + 240).trim() + suffix;
    return summary;
  };

const setSummaryPlaceholder = (payload, text, modifierClass) => {
    if (!payload.placeholder) {
      payload.placeholder = document.createElement("p");
      payload.item.appendChild(payload.placeholder);
    }

    const classes = ["kernel-search-summary", "kernel-search-summary-status"];
    if (modifierClass) classes.push(modifierClass);
    payload.placeholder.className = classes.join(" ");
    payload.placeholder.textContent = text;
  };

  const clearSummaryPlaceholder = (payload) => {
    if (!payload.placeholder) return;
    payload.placeholder.remove();
    payload.placeholder = null;
  };

  const loadDocumentText = (payload) => {
    if (documentTextCache.has(payload.requestUrl)) {
      return Promise.resolve(documentTextCache.get(payload.requestUrl));
    }

    const controller = typeof AbortController === "function"
      ? new AbortController()
      : null;
    payload.abortController = controller;

    return fetch(payload.requestUrl, controller ? { signal: controller.signal } : {})
      .then((response) => {
        if (!response.ok) {
          throw new Error(`Summary request failed: ${response.status}`);
        }
        return response.text();
      })
      .then((htmlText) => {
        documentTextCache.set(payload.requestUrl, htmlText);
        return htmlText;
      })
      .finally(() => {
        if (payload.abortController === controller) payload.abortController = null;
      });
  };

  const pushBest = (resultMap, result) => {
    const key = [result.kind, result.docName, result.anchor || "", result.title].join("|");
    const existing = resultMap.get(key);
    if (!existing || existing.score < result.score) resultMap.set(key, result);
  };

  // Query parsing, scoring, and deduplication.
  const buildQueryState = (query, exact) => {
    const rawTerms = splitQuery(query.trim());
    const rawTermsLower = rawTerms.map((term) => term.toLowerCase());
    const objectTerms = new Set(rawTermsLower);
    const highlightTerms = exact ? rawTermsLower : [];
    const searchTerms = new Set();
    const excludedTerms = new Set();

    if (!exact) {
      const stemmer = getStemmer();
      rawTerms.forEach((term) => {
        const lowered = term.toLowerCase();
        if (hasStopword(lowered) || /^\d+$/.test(term)) {
          return;
        }

        const word = stemmer.stemWord(lowered);
        if (!word) return;

        if (word[0] === "-") excludedTerms.add(word.substring(1));
        else {
          searchTerms.add(word);
          highlightTerms.push(lowered);
        }
      });
    } else {
      rawTermsLower.forEach((term) => searchTerms.add(term));
    }

    if (typeof SPHINX_HIGHLIGHT_ENABLED !== "undefined" && SPHINX_HIGHLIGHT_ENABLED) {
      localStorage.setItem("sphinx_highlight_terms", [...new Set(highlightTerms)].join(" "));
    }

    return {
      exact,
      query,
      queryLower: query.toLowerCase().trim(),
      rawTerms: rawTermsLower,
      objectTerms,
      searchTerms,
      excludedTerms,
      highlightTerms: [...new Set(highlightTerms)],
    };
  };

  const candidateMatches = (candidateLower, state) => {
    if (!state.queryLower) return false;
    if (state.exact) return candidateLower === state.queryLower;

    if (
      candidateLower.includes(state.queryLower)
      && state.queryLower.length >= Math.ceil(candidateLower.length / 2)
    ) {
      return true;
    }

    return state.rawTerms.length > 0
      && state.rawTerms.every((term) => candidateLower.includes(term));
  };

  const scoreLabelMatch = (candidateLower, state, baseScore, partialScore) => {
    if (state.exact) return baseScore + LABEL_MATCH_SCORES.exactMatchBonus;
    if (candidateLower === state.queryLower) {
      return baseScore + LABEL_MATCH_SCORES.exactCandidateBonus;
    }
    if (candidateLower.includes(state.queryLower)) {
      return Math.max(partialScore, Math.round((baseScore * state.queryLower.length) / candidateLower.length));
    }

    return partialScore * Math.max(1, state.rawTerms.filter((term) => candidateLower.includes(term)).length);
  };

  // Result collectors map Sphinx index structures to one result kind each.
  const collectObjectResults = (index, state, filters) => {
    const resultMap = new Map();
    const objects = index.objects || {};
    const objNames = index.objnames || {};
    const objTypes = index.objtypes || {};

    const addObjectResult = (prefix, name, match) => {
      const fileIndex = match[0];
      const typeIndex = match[1];
      const priority = match[2];
      const anchorValue = match[3];
      const docName = index.docnames[fileIndex];
      const fileName = index.filenames[fileIndex];
      const pageTitle = index.titles[fileIndex];
      const objectLabel = objNames[typeIndex] ? objNames[typeIndex][2] : "Object";
      const objectType = objTypes[typeIndex];

      if (!matchArea(docName, filters.area)) return;
      if (filters.objtype && filters.objtype !== objectType) return;

      const fullName = prefix ? prefix + "." + name : name;
      const fullNameLower = fullName.toLowerCase();
      const lastNameLower = fullNameLower.split(".").slice(-1)[0];
      const nameLower = name.toLowerCase();

      let score = 0;
      if (state.exact) {
        if (
          fullNameLower !== state.queryLower
          && lastNameLower !== state.queryLower
          && nameLower !== state.queryLower
        ) {
          return;
        }
        score = OBJECT_MATCH_SCORES.exact;
      } else {
        const haystack = `${fullName} ${objectLabel} ${pageTitle}`.toLowerCase();
        if (state.objectTerms.size === 0) return;
        if ([...state.objectTerms].some((term) => !haystack.includes(term))) return;
        const matchedNameTerms = state.rawTerms.filter(
          (term) =>
            fullNameLower.includes(term)
            || lastNameLower.includes(term)
            || nameLower.includes(term),
        ).length;

        if (
          fullNameLower === state.queryLower
          || lastNameLower === state.queryLower
          || nameLower === state.queryLower
        ) {
          score += OBJECT_MATCH_SCORES.exactNameBoost;
        } else if (
          lastNameLower.includes(state.queryLower)
          || nameLower.includes(state.queryLower)
        ) {
          score += OBJECT_MATCH_SCORES.partialShortName;
        } else if (fullNameLower.includes(state.queryLower)) {
          score += OBJECT_MATCH_SCORES.partialFullName;
        } else if (matchedNameTerms > 0) {
          score += matchedNameTerms * OBJECT_MATCH_SCORES.matchedNameTerm;
        } else {
          return;
        }
      }

      score += OBJECT_PRIORITY[priority] || 0;

      let anchor = anchorValue;
      if (anchor === "") anchor = fullName;
      else if (anchor === "-" && objNames[typeIndex]) anchor = objNames[typeIndex][1] + "-" + fullName;

      pushBest(resultMap, {
        kind: "object",
        docName,
        fileName,
        title: fullName,
        anchor: anchor ? "#" + anchor : "",
        description: `${objectLabel}, in ${pageTitle}`,
        score,
      });
    };

    Object.keys(objects).forEach((prefix) => {
      const group = objects[prefix];

      // Sphinx 3.x stores objects as name->tuple mappings; 4.x+ switched
      // to arrays with the display name appended as a fifth element.
      if (Array.isArray(group)) {
        group.forEach((match) => {
          addObjectResult(prefix, match[4], match);
        });
        return;
      }

      Object.entries(group || {}).forEach(([name, match]) => {
        addObjectResult(prefix, name, match);
      });
    });

    return [...resultMap.values()].sort(compareResults);
  };

  const collectSectionResults = (index, state, filters) => {
    const resultMap = new Map();
    const allTitles = index.alltitles || {};

    Object.entries(allTitles).forEach(([sectionTitle, entries]) => {
      const lowered = sectionTitle.toLowerCase().trim();
      if (!candidateMatches(lowered, state)) return;

      entries.forEach(([fileIndex, anchorId]) => {
        const docName = index.docnames[fileIndex];
        const fileName = index.filenames[fileIndex];
        const pageTitle = index.titles[fileIndex];
        if (!matchArea(docName, filters.area)) return;

        if (anchorId === null && sectionTitle === pageTitle) return;

        pushBest(resultMap, {
          kind: "title",
          docName,
          fileName,
          title: pageTitle !== sectionTitle ? `${pageTitle} > ${sectionTitle}` : sectionTitle,
          anchor: anchorId ? "#" + anchorId : "",
          description: pageTitle,
          score: scoreLabelMatch(
            lowered,
            state,
            LABEL_MATCH_SCORES.sectionBase,
            LABEL_MATCH_SCORES.sectionPartial,
          ),
        });
      });
    });

    return [...resultMap.values()].sort(compareResults);
  };

  const collectIndexResults = (index, state, filters) => {
    const resultMap = new Map();
    const entries = index.indexentries || {};

    Object.entries(entries).forEach(([entry, matches]) => {
      const lowered = entry.toLowerCase().trim();
      if (!candidateMatches(lowered, state)) return;

      matches.forEach(([fileIndex, anchorId, isMain]) => {
        const docName = index.docnames[fileIndex];
        const fileName = index.filenames[fileIndex];
        const pageTitle = index.titles[fileIndex];
        if (!matchArea(docName, filters.area)) return;

        let score = scoreLabelMatch(
          lowered,
          state,
          LABEL_MATCH_SCORES.indexBase,
          LABEL_MATCH_SCORES.indexPartial,
        );
        if (!isMain) score -= LABEL_MATCH_SCORES.secondaryIndexPenalty;

        pushBest(resultMap, {
          kind: "index",
          docName,
          fileName,
          title: entry,
          anchor: anchorId ? "#" + anchorId : "",
          description: pageTitle,
          score,
        });
      });
    });

    return [...resultMap.values()].sort(compareResults);
  };

  const collectTextResults = (index, state, filters) => {
    // Intersect per-word matches from the inverted index and keep the
    // best score contribution for each matched term per file.
    const resultMap = new Map();
    const terms = index.terms || {};
    const titleTerms = index.titleterms || {};
    const searchTerms = [...state.searchTerms];

    if (searchTerms.length === 0) return [];

    const scoreMap = new Map();
    const fileMap = new Map();

    searchTerms.forEach((word) => {
      const files = [];
      const candidates = [
        {
          files: hasOwn(terms, word) ? terms[word] : undefined,
          score: TEXT_MATCH_SCORES.term,
        },
        {
          files: hasOwn(titleTerms, word) ? titleTerms[word] : undefined,
          score: TEXT_MATCH_SCORES.titleTerm,
        },
      ];

      if (!state.exact && word.length > 2) {
        if (!hasOwn(terms, word)) {
          Object.keys(terms).forEach((term) => {
            if (term.includes(word)) {
              candidates.push({ files: terms[term], score: TEXT_MATCH_SCORES.partialTerm });
            }
          });
        }
        if (!hasOwn(titleTerms, word)) {
          Object.keys(titleTerms).forEach((term) => {
            if (term.includes(word)) {
              candidates.push({ files: titleTerms[term], score: TEXT_MATCH_SCORES.partialTitleTerm });
            }
          });
        }
      }

      if (candidates.every((candidate) => candidate.files === undefined)) return;

      candidates.forEach((candidate) => {
        if (candidate.files === undefined) return;

        let recordFiles = candidate.files;
        if (recordFiles.length === undefined) recordFiles = [recordFiles];
        files.push(...recordFiles);

        recordFiles.forEach((fileIndex) => {
          if (!scoreMap.has(fileIndex)) scoreMap.set(fileIndex, new Map());
          const currentScore = scoreMap.get(fileIndex).get(word) || 0;
          scoreMap.get(fileIndex).set(word, Math.max(currentScore, candidate.score));
        });
      });

      files.forEach((fileIndex) => {
        if (!fileMap.has(fileIndex)) fileMap.set(fileIndex, [word]);
        else if (!fileMap.get(fileIndex).includes(word)) fileMap.get(fileIndex).push(word);
      });
    });

    const filteredTermCount = state.exact
      ? searchTerms.length
      : searchTerms.filter((term) => term.length > 2).length;

    for (const [fileIndex, matchedWords] of fileMap.entries()) {
      const docName = index.docnames[fileIndex];
      const fileName = index.filenames[fileIndex];
      if (!matchArea(docName, filters.area)) continue;

      if (matchedWords.length !== searchTerms.length && matchedWords.length !== filteredTermCount) {
        continue;
      }

      if (
        [...state.excludedTerms].some(
          (term) =>
            terms[term] === fileIndex
            || titleTerms[term] === fileIndex
            || (terms[term] || []).includes(fileIndex)
            || (titleTerms[term] || []).includes(fileIndex),
        )
      ) {
        continue;
      }

      let score = Math.max(...matchedWords.map((word) => scoreMap.get(fileIndex).get(word)));
      if (state.exact && index.titles[fileIndex].toLowerCase() === state.queryLower) {
        score += TEXT_MATCH_SCORES.exactTitleBonus;
      }

      pushBest(resultMap, {
        kind: "text",
        docName,
        fileName,
        title: index.titles[fileIndex],
        anchor: "",
        description: null,
        score,
      });
    }

    return [...resultMap.values()].sort(compareResults);
  };

  const buildFilters = (state) => ({
    area: state.area,
    objtype: state.objtype,
  });

  // Rendering and lazy summary loading.
  const resetSummaryState = () => {
    summaryGeneration += 1;
    summaryQueue = [];
    activeFetchCount = 0;

    if (summaryViewportObserver) {
      summaryViewportObserver.disconnect();
      summaryViewportObserver = null;
    }
    summaryViewportRoot = null;

    summaryPayloads.forEach((payload) => {
      if (payload.status !== "loading") return;
      payload.loadToken += 1;
      payload.status = "idle";
      clearSummaryPlaceholder(payload);
      if (payload.abortController) {
        payload.abortController.abort();
        payload.abortController = null;
      }
    });
    summaryPayloads = [];
  };

  const finishSummaryLoad = (task) => {
    const payload = task.payload;
    activeFetchCount = Math.max(0, activeFetchCount - 1);
    if (payload.loadToken !== task.loadToken) {
      drainSummaryQueue();
      return;
    }
    drainSummaryQueue();
  };

  const markSummaryError = (payload) => {
    payload.status = "error";
    setSummaryPlaceholder(payload, "Summary unavailable.", "is-error");
  };

  const runSummaryLoad = (payload) => {
    if (payload.generation !== summaryGeneration || payload.status !== "queued") {
      return;
    }

    payload.status = "loading";
    setSummaryPlaceholder(payload, "Loading summary...", "is-loading");
    payload.loadToken += 1;
    const task = {
      loadToken: payload.loadToken,
      payload,
    };
    activeFetchCount += 1;
    loadDocumentText(payload)
      .then((htmlText) => {
        if (
          payload.loadToken !== task.loadToken
          || payload.generation !== summaryGeneration
          || payload.status !== "loading"
        ) {
          return;
        }

        const summary = makeSummary(htmlText, payload.keywords, payload.anchor);
        if (!summary) {
          markSummaryError(payload);
          return;
        }

        clearSummaryPlaceholder(payload);
        payload.item.appendChild(summary);
        payload.status = "done";
      })
      .catch(() => {
        if (payload.loadToken !== task.loadToken || payload.status !== "loading") return;
        markSummaryError(payload);
      })
      .finally(() => finishSummaryLoad(task));
  };

  const drainSummaryQueue = () => {
    while (activeFetchCount < SUMMARY_FETCH_BUDGET && summaryQueue.length) {
      const payload = summaryQueue.shift();
      if (!payload) break;
      if (payload.generation !== summaryGeneration || payload.status !== "queued") continue;
      runSummaryLoad(payload);
    }
  };

  const enqueueSummaryLoad = (payload) => {
    if (
      !payload
      || payload.generation !== summaryGeneration
      || payload.status !== "idle"
      || (pageSummaryLimitEnabled && payload.summaryIndex >= SUMMARY_RESULT_LIMIT)
    ) {
      return;
    }

    payload.status = "queued";
    summaryQueue.push(payload);
    drainSummaryQueue();
  };

  const cancelSummaryLoad = (payload) => {
    if (payload.status !== "queued" && payload.status !== "loading") return;
    payload.loadToken += 1;
    payload.status = "idle";
    clearSummaryPlaceholder(payload);
    if (payload.abortController) {
      payload.abortController.abort();
      payload.abortController = null;
    }
  };

  const boostSummaryPayload = (payload) => {
    if (payload.generation !== summaryGeneration) return;
    if (payload.status === "queued") {
      const index = summaryQueue.indexOf(payload);
      if (index > 0) {
        summaryQueue.splice(index, 1);
        summaryQueue.unshift(payload);
      }
      drainSummaryQueue();
    } else if (payload.status === "idle") {
      enqueueSummaryLoad(payload);
    }
  };

  const ensureViewportObserver = (rootElement) => {
    if (summaryViewportObserver && summaryViewportRoot === rootElement) {
      return summaryViewportObserver;
    }
    if (summaryViewportObserver) {
      summaryViewportObserver.disconnect();
      summaryViewportObserver = null;
    }
    summaryViewportRoot = rootElement;
    if (typeof IntersectionObserver !== "function") return null;

    summaryViewportObserver = new IntersectionObserver((entries) => {
      entries.forEach((entry) => {
        if (!entry.isIntersecting) return;
        const payload = summaryPayloads.find((p) => p.item === entry.target);
        if (payload) boostSummaryPayload(payload);
      });
    }, {
      root: rootElement,
      rootMargin: SUMMARY_VIEWPORT_MARGIN,
    });

    return summaryViewportObserver;
  };

  const activateSummaryLoads = (rootElement) => {
    const observer = ensureViewportObserver(rootElement);

    summaryQueue = summaryQueue.filter((payload) => {
      if (payload.generation !== summaryGeneration || payload.status !== "queued") return false;
      if (pageSummaryLimitEnabled && payload.summaryIndex >= SUMMARY_RESULT_LIMIT) {
        cancelSummaryLoad(payload);
        return false;
      }
      return true;
    });

    summaryPayloads.forEach((payload) => {
      if (payload.generation !== summaryGeneration) return;
      if (pageSummaryLimitEnabled && payload.summaryIndex >= SUMMARY_RESULT_LIMIT) {
        if (payload.status === "queued" || payload.status === "loading") {
          cancelSummaryLoad(payload);
        }
        return;
      }
      if (observer) observer.observe(payload.item);
      if (payload.status !== "idle") return;
      enqueueSummaryLoad(payload);
    });

    drainSummaryQueue();
  };

  const pauseSummaryLoads = () => {
    if (summaryViewportObserver) {
      summaryViewportObserver.disconnect();
      summaryViewportObserver = null;
    }
    summaryViewportRoot = null;
    summaryQueue = [];
    summaryPayloads.forEach((payload) => cancelSummaryLoad(payload));
  };

  const resetTabStripState = () => {
    if (!tabStripCleanup) return;
    tabStripCleanup();
    tabStripCleanup = null;
  };

  const bindTabStripShadows = (frame, scroller) => {
    resetTabStripState();

    const syncShadows = () => {
      const maxScrollLeft = Math.max(0, scroller.scrollWidth - scroller.clientWidth);
      const hasOverflow = maxScrollLeft > 1;
      frame.classList.toggle("has-left-shadow", hasOverflow && scroller.scrollLeft > 1);
      frame.classList.toggle(
        "has-right-shadow",
        hasOverflow && scroller.scrollLeft < maxScrollLeft - 1,
      );
    };

    scroller.addEventListener("scroll", syncShadows, { passive: true });
    if (typeof ResizeObserver === "function") {
      const resizeObserver = new ResizeObserver(syncShadows);
      resizeObserver.observe(scroller);
      tabStripCleanup = () => {
        scroller.removeEventListener("scroll", syncShadows);
        resizeObserver.disconnect();
      };
    } else {
      window.addEventListener("resize", syncShadows);
      tabStripCleanup = () => {
        scroller.removeEventListener("scroll", syncShadows);
        window.removeEventListener("resize", syncShadows);
      };
    }

    window.requestAnimationFrame(syncShadows);
  };

  const createResultItem = (result, keywords, summaryIndex) => {
    const urls = buildDocUrls(result.docName);
    const item = document.createElement("li");
    item.className = `kernel-search-result kind-${result.kind}`;

    const heading = item.appendChild(document.createElement("div"));
    heading.className = "kernel-search-result-heading";

    const link = heading.appendChild(document.createElement("a"));
    link.href = urls.linkUrl + result.anchor;
    link.dataset.score = String(result.score);
    link.textContent = result.title;

    const path = item.appendChild(document.createElement("div"));
    path.className = "kernel-search-path";
    path.textContent = result.fileName;

    if (result.description) {
      const meta = item.appendChild(document.createElement("div"));
      meta.className = "kernel-search-meta";
      meta.textContent = result.description;
    }

    if (result.kind === "text") {
      const payload = {
        abortController: null,
        anchor: result.anchor,
        generation: summaryGeneration,
        item,
        keywords,
        loadToken: 0,
        placeholder: null,
        requestUrl: urls.requestUrl,
        summaryIndex,
        status: "idle",
      };
      summaryPayloads.push(payload);
    }
    return item;
  };

  const renderResults = (state, resultsByKind) => {
    const container = document.getElementById("kernel-search-results");
    const totalResults = RESULT_KIND_ORDER.reduce(
      (count, kind) => count + resultsByKind[kind].length,
      0,
    );
    resetSummaryState();
    resetTabStripState();
    container.replaceChildren();

    const summary = document.createElement("p");
    summary.className = "kernel-search-status";
    if (!state.queryLower) {
      summary.textContent = "Enter a search query to browse kernel documentation.";
      container.appendChild(summary);
      return;
    }

    if (!totalResults) {
      summary.textContent =
        "No matching results were found for the current query and filters.";
      container.appendChild(summary);
      return;
    }

    summary.textContent =
      `Found ${totalResults} result${totalResults === 1 ? "" : "s"} for "${state.query}".`;
    container.appendChild(summary);

    const availableKinds = RESULT_KIND_ORDER.filter((kind) => resultsByKind[kind].length);
    const shell = container.appendChild(document.createElement("div"));
    shell.className = "kernel-search-results-shell";

    const tabFrame = shell.appendChild(document.createElement("div"));
    tabFrame.className = "kernel-search-tab-strip";

    const tabScroller = tabFrame.appendChild(document.createElement("div"));
    tabScroller.className = "kernel-search-tab-scroller";
    tabScroller.setAttribute("role", "tablist");
    tabScroller.setAttribute("aria-label", "Search result kinds");

    const toolRow = shell.appendChild(document.createElement("div"));
    toolRow.className = "kernel-search-panel-tools";
    toolRow.hidden = true;

    const summaryLimitLabel = toolRow.appendChild(document.createElement("label"));
    summaryLimitLabel.className = "kernel-search-checkbox kernel-search-summary-limit";
    summaryLimitLabel.hidden = !availableKinds.includes("text");

    const summaryLimitToggle = summaryLimitLabel.appendChild(document.createElement("input"));
    summaryLimitToggle.type = "checkbox";
    summaryLimitToggle.checked = pageSummaryLimitEnabled;

    const summaryLimitText = summaryLimitLabel.appendChild(document.createElement("span"));
    summaryLimitText.textContent = "Limit page summaries to first 50";

    const panels = shell.appendChild(document.createElement("div"));
    panels.className = "kernel-search-panels";

    const tabButtons = new Map();
    const tabPanels = new Map();

    const selectTab = (kind, focusTab) => {
      activeResultKind = kind;
      tabButtons.forEach((button, buttonKind) => {
        const active = buttonKind === kind;
        button.classList.toggle("is-active", active);
        button.setAttribute("aria-selected", active ? "true" : "false");
        button.tabIndex = active ? 0 : -1;
        if (active && focusTab) {
          button.focus();
          button.scrollIntoView({ block: "nearest", inline: "nearest" });
        }
      });

      tabPanels.forEach((panel, panelKind) => {
        const active = panelKind === kind;
        panel.hidden = !active;
        panel.classList.toggle("is-active", active);
      });

      toolRow.hidden = kind !== "text";

      if (kind === "text") {
        const panel = tabPanels.get("text");
        if (panel) activateSummaryLoads(panel);
      } else {
        pauseSummaryLoads();
      }
    };

    const handleTabKeydown = (event) => {
      const currentIndex = availableKinds.indexOf(activeResultKind);
      if (currentIndex === -1) return;

      let nextIndex = -1;
      switch (event.key) {
        case "ArrowLeft":
        case "ArrowUp":
          nextIndex = (currentIndex + availableKinds.length - 1) % availableKinds.length;
          break;
        case "ArrowRight":
        case "ArrowDown":
          nextIndex = (currentIndex + 1) % availableKinds.length;
          break;
        case "Home":
          nextIndex = 0;
          break;
        case "End":
          nextIndex = availableKinds.length - 1;
          break;
        default:
          return;
      }

      event.preventDefault();
      selectTab(availableKinds[nextIndex], true);
    };

    RESULT_KIND_ORDER.forEach((kind) => {
      const results = resultsByKind[kind];
      if (!results.length) return;

      const tab = tabScroller.appendChild(document.createElement("button"));
      tab.type = "button";
      tab.className = `kernel-search-tab kind-${kind}`;
      tab.id = `kernel-search-tab-${kind}`;
      tab.setAttribute("role", "tab");
      tab.setAttribute("aria-controls", `kernel-search-panel-${kind}`);
      tab.addEventListener("click", () => selectTab(kind, false));
      tab.addEventListener("keydown", handleTabKeydown);
      tabButtons.set(kind, tab);

      const label = tab.appendChild(document.createElement("span"));
      label.className = "kernel-search-tab-label";
      label.textContent = RESULT_KIND_LABELS[kind];

      const count = tab.appendChild(document.createElement("span"));
      count.className = "kernel-search-tab-count";
      count.textContent = String(results.length);

      const panel = panels.appendChild(document.createElement("section"));
      panel.className = `kernel-search-panel kind-${kind}`;
      panel.id = `kernel-search-panel-${kind}`;
      panel.setAttribute("role", "tabpanel");
      panel.setAttribute("aria-labelledby", tab.id);
      panel.hidden = true;
      tabPanels.set(kind, panel);

      const list = panel.appendChild(document.createElement("ol"));
      list.className = "kernel-search-list";

      results.forEach((result, index) => {
        list.appendChild(
          createResultItem(
            result,
            state.highlightTerms,
            index,
          ),
        );
      });
    });

    summaryLimitToggle.addEventListener("change", () => {
      pageSummaryLimitEnabled = summaryLimitToggle.checked;
      if (activeResultKind === "text") {
        const panel = tabPanels.get("text");
        if (panel) activateSummaryLoads(panel);
      }
    });

    bindTabStripShadows(tabFrame, tabScroller);
    const defaultKind = availableKinds.includes(activeResultKind)
      ? activeResultKind
      : availableKinds[0];
    selectTab(defaultKind, false);
  };

  // Form-state parsing, dynamic filter options, and page initialization.
  const populateAreaOptions = (select, state) => {
    const areas = new Set();
    window.Search._index.docnames.forEach((docName) => areas.add(getAreaValue(docName)));

    const options = [new Option("All documentation areas", "", false, !state.area)];
    [...areas]
      .sort((left, right) => {
        if (left === TOP_LEVEL_AREA) return -1;
        if (right === TOP_LEVEL_AREA) return 1;
        return left.localeCompare(right);
      })
      .forEach((area) => {
        options.push(new Option(getAreaLabel(area), area, false, area === state.area));
      });

    select.replaceChildren(...options);
  };

  const populateObjectTypeOptions = (select, state) => {
    const objTypes = window.Search._index.objtypes || {};
    const objNames = window.Search._index.objnames || {};
    const entries = Object.keys(objTypes)
      .map((key) => ({
        value: objTypes[key],
        label: objNames[key] ? objNames[key][2] : objTypes[key],
      }))
      .sort((left, right) => left.label.localeCompare(right.label));

    const seen = new Set();
    const options = [new Option("All object types", "", false, !state.objtype)];
    entries.forEach((entry) => {
      if (seen.has(entry.value)) return;
      seen.add(entry.value);
      options.push(new Option(entry.label, entry.value, false, entry.value === state.objtype));
    });

    select.replaceChildren(...options);
  };

  const parseState = () => {
    const params = new URLSearchParams(window.location.search);
    const kinds = params.getAll("kind").filter((kind) => RESULT_KIND_ORDER.includes(kind));

    return {
      query: params.get("q") || "",
      queryLower: (params.get("q") || "").toLowerCase().trim(),
      exact: params.get("exact") === "1",
      area: params.get("area") || "",
      objtype: params.get("objtype") || "",
      advanced: params.get("advanced") === "1",
      kinds: kinds.length ? new Set(kinds) : new Set(RESULT_KIND_ORDER),
    };
  };

  const shouldOpenAdvanced = (state) =>
    state.advanced
    || state.exact
    || state.area !== ""
    || state.objtype !== ""
    || RESULT_KIND_ORDER.some((kind) => !state.kinds.has(kind));

  const bindFormState = (state) => {
    document.getElementById("kernel-search-query").value = state.query;
    document.getElementById("kernel-search-exact").checked = state.exact;
    RESULT_KIND_ORDER.forEach((kind) => {
      const checkbox = document.getElementById(`kernel-search-kind-${kind}`);
      if (checkbox) checkbox.checked = state.kinds.has(kind);
    });

    const advanced = document.getElementById("kernel-search-advanced");
    const advancedFlag = document.getElementById("kernel-search-advanced-flag");
    const open = shouldOpenAdvanced(state);
    advanced.open = open;
    advancedFlag.disabled = !open;
    advanced.addEventListener("toggle", () => {
      advancedFlag.disabled = !advanced.open;
    });
  };

  const runSearch = () => {
    const baseState = parseState();
    bindFormState(baseState);
    populateAreaOptions(document.getElementById("kernel-search-area"), baseState);
    populateObjectTypeOptions(document.getElementById("kernel-search-objtype"), baseState);

    const queryState = buildQueryState(baseState.query, baseState.exact);
    const renderState = {
      ...baseState,
      highlightTerms: queryState.highlightTerms,
    };
    const filters = buildFilters(baseState);
    const resultsByKind = {
      object: [],
      title: [],
      index: [],
      text: [],
    };

    if (!baseState.queryLower) {
      renderResults(renderState, resultsByKind);
      return;
    }

    if (baseState.kinds.has("object")) {
      resultsByKind.object = collectObjectResults(window.Search._index, queryState, filters);
    }
    if (baseState.kinds.has("title")) {
      resultsByKind.title = collectSectionResults(window.Search._index, queryState, filters);
    }
    if (baseState.kinds.has("index")) {
      resultsByKind.index = collectIndexResults(window.Search._index, queryState, filters);
    }
    if (baseState.kinds.has("text")) {
      resultsByKind.text = collectTextResults(window.Search._index, queryState, filters);
    }

    renderResults(renderState, resultsByKind);
  };

  document.addEventListener("DOMContentLoaded", () => {
    const container = document.getElementById("kernel-search-results");
    if (!container) return;

    const progress = document.getElementById("search-progress");
    if (progress) progress.textContent = "Preparing search...";

    window.Search.whenReady(() => {
      if (progress) progress.textContent = "";
      runSearch();
    });
  });
})();
