#!/bin/bash
#set -x

# Default number of days to search
days=1

# Parse command line arguments
while [ "$#" -gt 0 ]; do
    case "$1" in
        --days)
            shift
            if [[ "$1" =~ ^[0-9]+$ ]]; then
                days="$1"
            else
                echo "Error: --days requires a numeric argument"
                exit 1
            fi
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--days N]"
            exit 1
            ;;
    esac
    shift
done

URL="https://lore.kernel.org/all/?q=s:%22GIT+PULL%22+AND+t:torvalds+AND+rt:${days}.day.ago...+AND+NOT+s:re:&x=A"

temp_file=$(mktemp)
curl -s "$URL" > "$temp_file"

count=$(grep -c "<entry>" "$temp_file")
echo "Number of pull requests in the last ${days} day(s): $count"

# Extract message URLs and filter out query parameters and #related links
message_urls=$(grep -o "http://lore.kernel.org/all/[^\"]*" "$temp_file" | grep -v "\\?" | grep -v "#related")

echo "Processing pull requests..."

count=0
while read -r message_url; do
    count=$((count + 1))
    echo "Pull request #$count: $message_url"

    message_content=$(mktemp)
    curl -s -L "$message_url" > "$message_content"

    email_content=$(cat "$message_content")

    # Extract and clean sender information
    from_line=$(echo "$email_content" | grep -o "From:.*" | head -1)
    from_line=$(echo "$from_line" | sed 's/&lt;/</g' | sed 's/&gt;/>/g' | sed 's/&#34;/"/g' | sed 's/&quot;/"/g')

    if [[ "$from_line" =~ From:[[:space:]]+(.*)[[:space:]]+\<([^>]+)\> ]]; then
        sender_name="${BASH_REMATCH[1]}"
        sender_email="${BASH_REMATCH[2]}"
        sender_name=$(echo "$sender_name" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
        sender_email=$(echo "$sender_email" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
        echo "  Sender: $sender_name <$sender_email>"
    else
        echo "  Sender: $(echo "$from_line" | sed 's/From: //')"
    fi

    found_repo=false
    repo=""
    branch=""

    # Try extraction methods in order of preference

    # 1. Extract repo from HTML links
    html_href_lines=$(echo "$email_content" | grep -n '<a[[:space:]]*href=".*git.*"')

    if [ -n "$html_href_lines" ]; then
        while read -r numbered_line; do
            line_num=$(echo "$numbered_line" | cut -d: -f1)
            line=$(echo "$numbered_line" | cut -d: -f2-)

            if [[ $line =~ href=\"([^\"]*gitlab[^\"]*|[^\"]*git[^\"]*|[^\"]*kernel\.org[^\"]*)\" ]]; then
                repo="${BASH_REMATCH[1]}"

                # Check for branch on same line or next line
                if [[ $line =~ \</a\>([[:space:]]*([[:alnum:]/_.-]+)) ]]; then
                    branch="${BASH_REMATCH[2]}"
                    echo "  Repository: $repo"
                    echo "  Branch/Tag: $branch"
                    found_repo=true
                    break
                else
                    next_line_num=$((line_num + 1))
                    next_line=$(echo "$email_content" | sed -n "${next_line_num}p")
                    next_line=$(echo "$next_line" | sed 's/^[[:space:]]*//' | sed 's/[[:space:]]*$//')

                    if [[ $next_line =~ ^[[:alnum:]/_.-]+$ ]]; then
                        branch="$next_line"
                        echo "  Repository: $repo"
                        echo "  Branch/Tag: $branch"
                        found_repo=true
                        break
                    elif [ "$found_repo" = false ]; then
                        repo_no_branch=$repo
                        line_no_branch=$line
                    fi
                fi
            fi
        done <<< "$html_href_lines"
    fi

    # 2. Extract repo from plain text if not found in HTML
    if [ "$found_repo" = false ]; then
        repo_lines=$(echo "$email_content" | grep -n -i "git://\|https://git\|git@" | grep -v "href=")

        if [ -n "$repo_lines" ]; then
            while read -r numbered_line; do
                line_num=$(echo "$numbered_line" | cut -d: -f1)
                line=$(echo "$numbered_line" | cut -d: -f2-)

                if [[ $line =~ (git://|ssh://git|https://git|git@)[^[:space:]]+(/[^[:space:]]+)+ ]]; then
                    repo="${BASH_REMATCH[0]}"
                    repo=$(echo "$repo" | sed 's/[,.\\]$//' | sed 's/[[:space:]]*$//')

                    if [[ $line =~ $repo[[:space:]]+([[:alnum:]/_.-]+) ]]; then
                        branch="${BASH_REMATCH[1]}"
                        echo "  Repository: $repo"
                        echo "  Branch/Tag: $branch"
                        found_repo=true
                        break
                    else
                        next_line_num=$((line_num + 1))
                        next_line=$(echo "$email_content" | sed -n "${next_line_num}p")
                        next_line=$(echo "$next_line" | sed 's/^[[:space:]]*//' | sed 's/[[:space:]]*$//')

                        if [[ $next_line =~ ^[[:alnum:]/_.-]+$ ]]; then
                            branch="$next_line"
                            echo "  Repository: $repo"
                            echo "  Branch/Tag: $branch"
                            found_repo=true
                            break
                        elif [ "$found_repo" = false ]; then
                            repo_no_branch=$repo
                            line_no_branch=$line
                        fi
                    fi
                fi
            done <<< "$repo_lines"
        fi
    fi

    # 3. Try "available in the Git repository at:" section
    if [ "$found_repo" = false ]; then
        main_repo_section=$(echo "$email_content" | grep -A 10 "available in the Git repository at")

        if [ -n "$main_repo_section" ]; then
            if [[ $main_repo_section =~ href=\"([^\"]*gitlab[^\"]*|[^\"]*git[^\"]*|[^\"]*kernel\.org[^\"]*) ]]; then
                repo="${BASH_REMATCH[1]}"
                echo "  Repository: $repo"
                found_repo=true

                tags_line=$(echo "$main_repo_section" | grep -o "tags/[[:alnum:]/_.-]*" | head -1)
                if [ -n "$tags_line" ]; then
                    branch="$tags_line"
                    echo "  Branch/Tag: $branch"
                fi
            fi
        fi
    fi

    # 4. Use repo without branch if that's all we found
    if [ "$found_repo" = false ] && [ -n "${repo_no_branch:-}" ]; then
        repo="$repo_no_branch"
        echo "  Repository: $repo"
        echo "  Context: $line_no_branch"
        found_repo=true
    fi

    if [ "$found_repo" = false ]; then
        echo "  No repository URL found in this pull request."
    else
        # Convert ssh URLs to git URLs for verification
        verification_repo="$repo"

        # Handle different git URL formats for kernel.org
        if [[ "$verification_repo" =~ ^ssh://git@gitolite\.kernel\.org(.*) ]]; then
            verification_repo="git://git.kernel.org${BASH_REMATCH[1]}"
            echo "  Using git URL for verification: $verification_repo"
        fi

        if [[ "$verification_repo" =~ ^git@gitolite\.kernel\.org:(.*) ]]; then
            verification_repo="git://git.kernel.org/${BASH_REMATCH[1]}"
            echo "  Using git URL for verification: $verification_repo"
        fi

        if [ -n "$verification_repo" ] && [ -n "$branch" ]; then
            # Try fetching, first with tags/ prefix if needed
            fetch_ref="$branch"
            if [[ ! "$branch" =~ ^(refs/|tags/) ]] && [[ ! "$branch" =~ ^remotes/ ]]; then
                fetch_ref="tags/$branch"
            fi

            echo "  Fetching: git fetch \"$verification_repo\" \"$fetch_ref\""
            if git fetch "$verification_repo" "$fetch_ref" 2>/dev/null; then
                echo "  Fetch: ✅ Successfully fetched"

                # Check if there are any commits to verify
                commit_hashes=$(git rev-list --no-merges origin/master..FETCH_HEAD 2>/dev/null)

                if [ -z "$commit_hashes" ]; then
                    echo "  ℹ️ No new commits found. Pull request likely already merged."
                else
                    total_commits=$(echo "$commit_hashes" | wc -l)
                    echo "  Checking maintainer status for $total_commits commit(s)..."

                    # Array to store problematic commits
                    problematic_commits=()
                    # Array to store commits where sender is not maintainer but a signer is
                    sender_not_maintainer_commits=()

                    # Check each commit silently
                    while read -r commit_hash; do
                        [ -z "$commit_hash" ] && continue

                        commit_msg=$(git log -1 --pretty=format:"%h %s" "$commit_hash")

                        if [ -f "scripts/get_maintainer.pl" ]; then
                            maintainers=$(git show "$commit_hash" | ./scripts/get_maintainer.pl)
                            signoffs=$(git show -s --format=%b "$commit_hash" | grep -i "Signed-off-by:" | sed 's/^[[:space:]]*Signed-off-by:[[:space:]]*//')

                            valid_maintainer=false
                            sender_is_maintainer=false

                            # Check if sender is a maintainer
                            if echo "$maintainers" | grep -q "$sender_email" || echo "$maintainers" | grep -q "$sender_name"; then
                                valid_maintainer=true
                                sender_is_maintainer=true
                            else
                                # Check if any signoff person is a maintainer
                                while read -r signoff; do
                                    [ -z "$signoff" ] && continue

                                    # Extract name and email from signoff
                                    if [[ "$signoff" =~ (.*)[[:space:]]+\<([^>]+)\> ]]; then
                                        signer_name="${BASH_REMATCH[1]}"
                                        signer_email="${BASH_REMATCH[2]}"
                                        signer_name=$(echo "$signer_name" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
                                        signer_email=$(echo "$signer_email" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')

                                        if echo "$maintainers" | grep -q "$signer_email" || echo "$maintainers" | grep -q "$signer_name"; then
                                            valid_maintainer=true
                                            break
                                        fi
                                    fi
                                done <<< "$signoffs"
                            fi

                            # Add to problematic commits if no valid maintainer found
                            if [ "$valid_maintainer" = false ]; then
                                problematic_commits+=("$commit_msg")
                            # Track commits where sender is not a maintainer but a signer is
                            elif [ "$sender_is_maintainer" = false ]; then
                                sender_not_maintainer_commits+=("$commit_msg")
                            fi
                        fi
                    done <<< "$commit_hashes"

                    # Display results based on problematic commits
                    if [ ${#problematic_commits[@]} -eq 0 ]; then
                        echo "  ✅ Maintainer verification: Sender or a signer is listed as maintainer for all commits"

                        # Add warning if we found commits where sender is not a maintainer
                        if [ ${#sender_not_maintainer_commits[@]} -gt 0 ]; then
                            echo "  ⚠️  Warning: Sender is NOT listed as maintainer for these commits (but a signer is):"
                            for commit in "${sender_not_maintainer_commits[@]}"; do
                                echo "    - $commit"
                            done
                        fi
                    else
                        echo "  ❌ Maintainer verification: Neither sender nor any signers are listed as maintainers for these commits:"
                        for commit in "${problematic_commits[@]}"; do
                            echo "    - $commit"
                        done
                    fi
                fi
            else
                # Try without tags/ prefix if the first attempt failed
                if [[ "$fetch_ref" == tags/* ]]; then
                    fetch_ref="${branch}"
                    echo "  Fetching: git fetch \"$verification_repo\" \"$fetch_ref\""
                    if git fetch "$verification_repo" "$fetch_ref" 2>/dev/null; then
                        echo "  Fetch: ✅ Successfully fetched"

                        # Check if there are any commits to verify
                        commit_hashes=$(git rev-list --no-merges origin/master..FETCH_HEAD 2>/dev/null)

                        if [ -z "$commit_hashes" ]; then
                            echo "  ℹ️ No new commits found. Pull request likely already merged."
                        else
                            total_commits=$(echo "$commit_hashes" | wc -l)
                            echo "  Checking maintainer status for $total_commits commit(s)..."

                            # Array to store problematic commits
                            problematic_commits=()
                            # Array to store commits where sender is not maintainer but a signer is
                            sender_not_maintainer_commits=()

                            # Check each commit silently
                            while read -r commit_hash; do
                                [ -z "$commit_hash" ] && continue

                                commit_msg=$(git log -1 --pretty=format:"%h %s" "$commit_hash")

                                if [ -f "scripts/get_maintainer.pl" ]; then
                                    maintainers=$(git show "$commit_hash" | ./scripts/get_maintainer.pl)
                                    signoffs=$(git show -s --format=%b "$commit_hash" | grep -i "Signed-off-by:" | sed 's/^[[:space:]]*Signed-off-by:[[:space:]]*//')

                                    valid_maintainer=false
                                    sender_is_maintainer=false

                                    # Check if sender is a maintainer
                                    if echo "$maintainers" | grep -q "$sender_email" || echo "$maintainers" | grep -q "$sender_name"; then
                                        valid_maintainer=true
                                        sender_is_maintainer=true
                                    else
                                        # Check if any signoff person is a maintainer
                                        while read -r signoff; do
                                            [ -z "$signoff" ] && continue

                                            # Extract name and email from signoff
                                            if [[ "$signoff" =~ (.*)[[:space:]]+\<([^>]+)\> ]]; then
                                                signer_name="${BASH_REMATCH[1]}"
                                                signer_email="${BASH_REMATCH[2]}"
                                                signer_name=$(echo "$signer_name" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
                                                signer_email=$(echo "$signer_email" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')

                                                if echo "$maintainers" | grep -q "$signer_email" || echo "$maintainers" | grep -q "$signer_name"; then
                                                    valid_maintainer=true
                                                    break
                                                fi
                                            fi
                                        done <<< "$signoffs"
                                    fi

                                    # Add to problematic commits if no valid maintainer found
                                    if [ "$valid_maintainer" = false ]; then
                                        problematic_commits+=("$commit_msg")
                                    # Track commits where sender is not a maintainer but a signer is
                                    elif [ "$sender_is_maintainer" = false ]; then
                                        sender_not_maintainer_commits+=("$commit_msg")
                                    fi
                                fi
                            done <<< "$commit_hashes"

                            # Display results based on problematic commits
                            if [ ${#problematic_commits[@]} -eq 0 ]; then
                                echo "  ✅ Maintainer verification: Sender or a signer is listed as maintainer for all commits"

                                # Add warning if we found commits where sender is not a maintainer
                                if [ ${#sender_not_maintainer_commits[@]} -gt 0 ]; then
                                    echo "  ⚠️ Warning: Sender is NOT listed as maintainer for these commits (but a signer is):"
                                    for commit in "${sender_not_maintainer_commits[@]}"; do
                                        echo "    - $commit"
                                    done
                                fi
                            else
                                echo "  ❌ Maintainer verification: Neither sender nor any signers are listed as maintainers for these commits:"
                                for commit in "${problematic_commits[@]}"; do
                                    echo "    - $commit"
                                done
                            fi
                        fi
                    else
                        echo "  Fetch: ❌ Failed to fetch"
                    fi
                else
                    echo "  Fetch: ❌ Failed to fetch"
                fi
            fi
        elif [ -n "$verification_repo" ]; then
            # If we only have the repository but no branch/tag, just verify the repository exists
            echo "  Verifying: git ls-remote --exit-code \"$verification_repo\""
            if git ls-remote --exit-code "$verification_repo" > /dev/null 2>&1; then
                echo "  Verification: ✅ Repository exists"
            else
                echo "  Verification: ❌ Could not access repository"
            fi
        fi
    fi

    rm "$message_content"

    echo "------------------------"
done <<< "$message_urls"

rm "$temp_file"
