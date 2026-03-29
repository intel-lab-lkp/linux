// SPDX-License-Identifier: GPL-2.0

(function () {
    const BUTTON_LABEL = "Copy code";
    const COPIED_LABEL = "Copied";
    const FAILED_LABEL = "Copy failed";
    const RESET_DELAY_MS = 2000;

    const COPY_ICON = `
        <svg viewBox="0 0 24 24" aria-hidden="true" fill="none"
             stroke="currentColor" stroke-width="2"
             stroke-linecap="round" stroke-linejoin="round">
            <g transform="translate(24 0) scale(-1 1)">
                <rect width="14" height="14" x="8" y="8" rx="2" ry="2"></rect>
                <path d="M4 16c-1.1 0-2-.9-2-2V4c0-1.1.9-2 2-2h10c1.1 0 2 .9 2 2"></path>
            </g>
        </svg>`;

    const COPIED_ICON = '<span aria-hidden="true">✓</span>';
    const FAILED_ICON = '<span aria-hidden="true">×</span>';

    function resetButtonState(button, status) {
        button.dataset.copyState = "idle";
        button.setAttribute("aria-label", BUTTON_LABEL);
        button.setAttribute("title", BUTTON_LABEL);
        button.innerHTML = COPY_ICON;
        status.textContent = "";
    }

    function setButtonState(button, status, state, label, icon) {
        button.dataset.copyState = state;
        button.setAttribute("aria-label", label);
        button.setAttribute("title", label);
        button.innerHTML = icon;
        status.textContent = label;

        if (button.resetTimer) {
            window.clearTimeout(button.resetTimer);
        }

        button.resetTimer = window.setTimeout(function () {
            resetButtonState(button, status);
        }, RESET_DELAY_MS);
    }

    async function copyText(text) {
        if (navigator.clipboard && navigator.clipboard.writeText) {
            try {
                await navigator.clipboard.writeText(text);
                return true;
            } catch (error) {
                /* Fall back to execCommand below. */
            }
        }

        /* Fall back for browsers where the async clipboard API is unavailable. */
        const textarea = document.createElement("textarea");
        textarea.value = text;
        textarea.setAttribute("readonly", "");
        textarea.style.position = "fixed";
        textarea.style.left = "-9999px";
        document.body.appendChild(textarea);
        textarea.select();

        try {
            return document.execCommand("copy");
        } catch (error) {
            return false;
        } finally {
            document.body.removeChild(textarea);
        }
    }

    function hideVisibleButtons(exceptWrapper) {
        document
            .querySelectorAll("div.highlight.kernel-copy-visible")
            .forEach(function (wrapper) {
                if (wrapper !== exceptWrapper) {
                    wrapper.classList.remove("kernel-copy-visible");
                }
            });
    }

    function addCopyButton(wrapper) {
        const pre = wrapper.querySelector("pre");

        if (!pre || wrapper.querySelector(":scope > button.kernel-copy-button")) {
            return;
        }

        const button = document.createElement("button");
        const status = document.createElement("span");

        button.className = "kernel-copy-button";
        button.type = "button";
        button.innerHTML = COPY_ICON;
        resetButtonState(button, status);

        status.className = "kernel-visually-hidden";
        status.setAttribute("aria-live", "polite");
        status.setAttribute("aria-atomic", "true");

        button.addEventListener("click", async function () {
            const ok = await copyText(pre.textContent || "");

            if (ok) {
                setButtonState(button, status, "copied", COPIED_LABEL, COPIED_ICON);
            } else {
                setButtonState(button, status, "error", FAILED_LABEL, FAILED_ICON);
            }
        });

        wrapper.appendChild(button);
        wrapper.appendChild(status);
        wrapper.classList.add("kernel-copy-block");
    }

    function initCopyButtons() {
        document.querySelectorAll("div.highlight").forEach(addCopyButton);

        document.addEventListener("pointerdown", function (event) {
            /* Hover already handles mouse users; this is for touch-style reveal. */
            if (event.pointerType === "mouse") {
                return;
            }

            const wrapper = event.target.closest("div.highlight.kernel-copy-block");
            hideVisibleButtons(wrapper);

            if (wrapper) {
                wrapper.classList.add("kernel-copy-visible");
            }
        });

        document.addEventListener("keydown", function (event) {
            if (event.key === "Escape") {
                hideVisibleButtons(null);
            }
        });
    }

    if (document.readyState === "loading") {
        document.addEventListener("DOMContentLoaded", initCopyButtons, { once: true });
    } else {
        initCopyButtons();
    }
})();
