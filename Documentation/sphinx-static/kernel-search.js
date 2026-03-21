"use strict";

(() => {
  const RESULT_KIND_ORDER = ["object", "title", "index", "text"];
  const RESULT_KIND_LABELS = {
    object: "Symbols",
    title: "Sections",
    index: "Index entries",
    text: "Pages",
  };
  const TOP_LEVEL_AREA = "__top_level__";
  const OBJECT_PRIORITY = {
    0: 15,
    1: 5,
    2: -5,
  };
  const SUMMARY_ROOT_MARGIN = "300px 0px";
  const documentTextCache = new Map();
  const summaryTargets = new WeakMap();
  let summaryObserver = null;

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

  const splitQuery = (query) =>
    query
      .split(/[^\p{Letter}\p{Number}_\p{Emoji_Presentation}]+/gu)
      .filter((term) => term);

  const getStemmer = () =>
    typeof Stemmer === "function" ? new Stemmer() : { stemWord: (word) => word };

  const hasOwn = (object, key) =>
    Object.prototype.hasOwnProperty.call(object, key);

  const getContentRoot = () =>
    document.documentElement.dataset.content_root
    || (typeof DOCUMENTATION_OPTIONS !== "undefined" ? DOCUMENTATION_OPTIONS.URL_ROOT || "" : "");

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

  const loadDocumentText = (requestUrl) => {
    if (!documentTextCache.has(requestUrl)) {
      documentTextCache.set(
        requestUrl,
        fetch(requestUrl)
          .then((response) => (response.ok ? response.text() : ""))
          .catch(() => ""),
      );
    }

    return documentTextCache.get(requestUrl);
  };

  const pushBest = (resultMap, result) => {
    const key = [result.kind, result.docName, result.anchor || "", result.title].join("|");
    const existing = resultMap.get(key);
    if (!existing || existing.score < result.score) resultMap.set(key, result);
  };

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
        if ((typeof stopwords !== "undefined" && stopwords.has(lowered)) || /^\d+$/.test(term)) {
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
    if (state.exact) return baseScore + 20;
    if (candidateLower === state.queryLower) return baseScore + 10;
    if (candidateLower.includes(state.queryLower)) {
      return Math.max(partialScore, Math.round((baseScore * state.queryLower.length) / candidateLower.length));
    }

    return partialScore * Math.max(1, state.rawTerms.filter((term) => candidateLower.includes(term)).length);
  };

  const collectObjectResults = (index, state, filters) => {
    const resultMap = new Map();
    const objects = index.objects || {};
    const objNames = index.objnames || {};
    const objTypes = index.objtypes || {};

    Object.keys(objects).forEach((prefix) => {
      objects[prefix].forEach((match) => {
        const fileIndex = match[0];
        const typeIndex = match[1];
        const priority = match[2];
        const anchorValue = match[3];
        const name = match[4];
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
          score = 120;
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
            score += 11;
          } else if (
            lastNameLower.includes(state.queryLower)
            || nameLower.includes(state.queryLower)
          ) {
            score += 6;
          } else if (fullNameLower.includes(state.queryLower)) {
            score += 4;
          } else if (matchedNameTerms > 0) {
            score += matchedNameTerms;
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
          score: scoreLabelMatch(lowered, state, 15, 7),
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

        let score = scoreLabelMatch(lowered, state, 20, 8);
        if (!isMain) score -= 5;

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
          score: 5,
        },
        {
          files: hasOwn(titleTerms, word) ? titleTerms[word] : undefined,
          score: 15,
        },
      ];

      if (!state.exact && word.length > 2) {
        if (!hasOwn(terms, word)) {
          Object.keys(terms).forEach((term) => {
            if (term.includes(word)) candidates.push({ files: terms[term], score: 2 });
          });
        }
        if (!hasOwn(titleTerms, word)) {
          Object.keys(titleTerms).forEach((term) => {
            if (term.includes(word)) candidates.push({ files: titleTerms[term], score: 7 });
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
      if (state.exact && index.titles[fileIndex].toLowerCase() === state.queryLower) score += 10;

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

  const ensureSummaryObserver = () => {
    if (summaryObserver || typeof IntersectionObserver !== "function") return summaryObserver;

    summaryObserver = new IntersectionObserver((entries) => {
      entries.forEach((entry) => {
        if (!entry.isIntersecting) return;

        const target = entry.target;
        summaryObserver.unobserve(target);
        const payload = summaryTargets.get(target);
        if (!payload || payload.loaded) return;

        payload.loaded = true;
        loadDocumentText(payload.requestUrl).then((htmlText) => {
          if (!htmlText) return;

          const summary = makeSummary(htmlText, payload.keywords, payload.anchor);
          if (!summary) return;

          payload.item.appendChild(summary);
        });
      });
    }, { rootMargin: SUMMARY_ROOT_MARGIN });

    return summaryObserver;
  };

  const queueSummaryLoad = (result, item, keywords) => {
    const urls = buildDocUrls(result.docName);
    const payload = {
      anchor: result.anchor,
      item,
      keywords,
      loaded: false,
      requestUrl: urls.requestUrl,
    };

    const observer = ensureSummaryObserver();
    if (!observer) {
      payload.loaded = true;
      loadDocumentText(payload.requestUrl).then((htmlText) => {
        if (!htmlText) return;

        const summary = makeSummary(htmlText, keywords, result.anchor);
        if (summary) item.appendChild(summary);
      });
      return;
    }

    summaryTargets.set(item, payload);
    observer.observe(item);
  };

  const createResultItem = (result, keywords) => {
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
      queueSummaryLoad(result, item, keywords);
    }
    return item;
  };

  const renderResults = (state, resultsByKind) => {
    const container = document.getElementById("kernel-search-results");
    const totalResults = RESULT_KIND_ORDER.reduce(
      (count, kind) => count + resultsByKind[kind].length,
      0,
    );
    if (summaryObserver) {
      summaryObserver.disconnect();
      summaryObserver = null;
    }
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

    RESULT_KIND_ORDER.forEach((kind) => {
      const results = resultsByKind[kind];
      if (!results.length) return;

      const group = container.appendChild(document.createElement("details"));
      group.className = `kernel-search-group kind-${kind}`;
      group.open = true;

      const summary = group.appendChild(document.createElement("summary"));
      summary.className = "kernel-search-group-summary";

      const heading = summary.appendChild(document.createElement("h2"));
      heading.className = "kernel-search-group-title";
      heading.textContent = RESULT_KIND_LABELS[kind];

      const count = summary.appendChild(document.createElement("span"));
      count.className = "kernel-search-group-count";
      count.textContent = `(${results.length})`;

      const list = group.appendChild(document.createElement("ol"));
      list.className = "kernel-search-list";

      results.forEach((result) => {
        list.appendChild(createResultItem(result, state.highlightTerms));
      });
    });
  };

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
    const filters = buildFilters(baseState);
    const resultsByKind = {
      object: [],
      title: [],
      index: [],
      text: [],
    };

    if (!baseState.queryLower) {
      renderResults(baseState, resultsByKind);
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

    renderResults(baseState, resultsByKind);
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
