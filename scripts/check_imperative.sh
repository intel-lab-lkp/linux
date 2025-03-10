#!/bin/bash

# check_imperative.sh
# This script checks if text is in imperative voice using various LLM providers
# If not, it rewrites the text in imperative voice while preserving all details

set -e

# Display usage information
usage()
{
	echo "Usage: $0 [--debug] [--pr] [--llm <provider_or_script>] <file_path|url|message-id> [output_file]"
	echo "  --debug                 : Show full analysis including thinking process"
	echo "  --pr                    : Input is a git pull request email; extract changes before converting"
	echo "                            With --pr, you can provide:"
	echo "                             - URL to a mailing list (e.g., lore.kernel.org)"
	echo "                             - message-id (e.g., 20250122112712.5c992f86@gandalf.local.home)"
	echo "  --llm <provider_or_script> : LLM provider to use or custom script path:"
	echo "                           - claude (default): Use Claude 3.7 Sonnet"
	echo "                           - openai: Use OpenAI GPT-4o"
	echo "                           - huggingface: Use Meta's Llama 3.1 70B Instruct"
	echo "                           - /path/to/script: Use custom script"
	echo "  <file_path|url|message-id> : Path to the text file to check, URL to PR email, or message-id"
	echo "  [output_file]           : Optional. Path to save the rewritten text (if needed)"
	echo
	echo "Environment variables:"
	echo "  CLAUDE_API_KEY    : Required for Claude API access"
	echo "  OPENAI_API_KEY    : Required for OpenAI API access"
	echo "  HF_API_KEY        : Required for HuggingFace API access"
	exit 1
}

# Check if required API key is set
check_api_key()
{
	local provider=$1
	local key_var=""
	local key_name=""

	case "$provider" in
		claude)
			key_var="CLAUDE_API_KEY"
			key_name="Claude API key"
			;;
		openai)
			key_var="OPENAI_API_KEY"
			key_name="OpenAI API key"
			;;
		huggingface)
			key_var="HF_API_KEY"
			key_name="HuggingFace API key"
			;;
		*)
			return 0 # No check needed for custom LLM
			;;
	esac

	if [ -z "${!key_var}" ]; then
		echo "Error: $key_var environment variable is not set"
		echo "Please set your $key_name with: export $key_var='your-api-key'"
		exit 1
	fi
}

# Parse command line arguments
DEBUG_MODE=false
PR_MODE=false
PROVIDER="claude"  # Default to Claude
CUSTOM_LLM=""
ARGS=()

while [[ $# -gt 0 ]]; do
	case "$1" in
		--debug)
			DEBUG_MODE=true
			shift
			;;
		--pr)
			PR_MODE=true
			shift
			;;
		--llm)
			if [ -n "$2" ] && [[ "$2" != -* ]]; then
				case "$2" in
					claude|openai|huggingface)
						PROVIDER="$2"
						;;
					*)
						# Check if it's a file path (custom script)
						if [ -f "$2" ]; then
							PROVIDER="custom"
							CUSTOM_LLM="$2"
						else
							echo "Error: Invalid LLM option '$2'. Must be claude, openai, huggingface, or a valid script path."
							usage
						fi
						;;
				esac
				shift 2
			else
				echo "Error: --llm requires a provider name or script path"
				usage
			fi
			;;
		-h|--help)
			usage
			;;
		*)
			ARGS+=("$1")
			shift
			;;
	esac
done

# Restore positional arguments
set -- "${ARGS[@]}"

# Check if required arguments are provided
if [ $# -lt 1 ]; then
	usage
fi

# Check if provider is valid
if [[ ! "$PROVIDER" =~ ^(claude|openai|huggingface|custom)$ ]]; then
	echo "Error: Invalid provider '$PROVIDER'. This is an internal error."
	exit 1
fi

# Check if custom LLM script is executable
if [ "$PROVIDER" = "custom" ]; then
	if [ ! -x "$CUSTOM_LLM" ]; then
		echo "Error: Custom LLM script '$CUSTOM_LLM' is not executable"
		echo "Please make it executable with: chmod +x $CUSTOM_LLM"
		exit 1
	fi
fi

# Check if jq is installed
if ! command -v jq &> /dev/null; then
	echo "Error: This script requires 'jq' but it's not installed."
	echo "Please install it using your package manager."
	echo "For example: sudo apt install jq (Debian/Ubuntu) or brew install jq (macOS)"
	exit 1
fi

INPUT_FILE="$1"
OUTPUT_FILE="${2:-}"

# Handle URLs or message-ids for PR mode
if [ "$PR_MODE" = true ]; then
	# Check if input looks like a message-id (contains @ symbol but doesn't start with http)
	if [[ "$INPUT_FILE" == *@* ]] && [[ ! "$INPUT_FILE" =~ ^https?:// ]]; then
		if [ "$DEBUG_MODE" = true ]; then
			echo "Detected message-id: $INPUT_FILE"
			echo "Converting to lore.kernel.org URL..."
		fi

		# Convert message-id to lore.kernel.org URL
		INPUT_FILE="https://lore.kernel.org/all/$INPUT_FILE/"

		if [ "$DEBUG_MODE" = true ]; then
			echo "Resolved URL: $INPUT_FILE"
		fi
	fi

	# Now handle as URL if it starts with http/https
	if [[ "$INPUT_FILE" =~ ^https?:// ]]; then
		if [ "$DEBUG_MODE" = true ]; then
			echo "Detected URL: $INPUT_FILE"
			echo "Downloading email content..."
		fi

		# Create a temporary file for the URL content
		TEMP_FILE=$(mktemp /tmp/check_imperative.XXXXXX)

		# Download the URL content
		if ! curl -s -L "$INPUT_FILE" > "$TEMP_FILE"; then
			echo "Error: Failed to download URL: $INPUT_FILE"
			rm -f "$TEMP_FILE"
			exit 1
		fi

		# Check if the download succeeded and has content
		if [ ! -s "$TEMP_FILE" ]; then
			echo "Error: Downloaded file is empty. Please check the URL: $INPUT_FILE"
			rm -f "$TEMP_FILE"
			exit 1
		fi

		if [ "$DEBUG_MODE" = true ]; then
			echo "Downloaded $(wc -l < "$TEMP_FILE") lines from URL"
		fi

		# Use the downloaded file as input
		INPUT_FILE="$TEMP_FILE"
		URL_CLEANUP_NEEDED=true
	else
		# Check if input file exists (for non-URL inputs)
		if [ ! -f "$INPUT_FILE" ]; then
			echo "Error: Input file '$INPUT_FILE' not found"
			exit 1
		fi
		URL_CLEANUP_NEEDED=false
	fi
else
	# For non-PR mode, just check if the file exists
	if [ ! -f "$INPUT_FILE" ]; then
		echo "Error: Input file '$INPUT_FILE' not found"
		exit 1
	fi
	URL_CLEANUP_NEEDED=false
fi

# Check if appropriate API key is set (not needed for custom LLM)
if [ "$PROVIDER" != "custom" ]; then
	check_api_key "$PROVIDER"
fi

# Read the content of the input file
FILE_CONTENT=$(cat "$INPUT_FILE")

# Clean up temporary URL file if needed
if [ "$URL_CLEANUP_NEEDED" = true ]; then
	rm -f "$INPUT_FILE"
fi

# If in PR mode, extract and convert the relevant changes at once
if [ "$PR_MODE" = true ]; then
	if [ "$DEBUG_MODE" = true ]; then
		echo "Extracting changes from pull request email and converting to imperative voice..."
	fi

	# Combined prompt that extracts changes and converts to imperative voice in one call
	COMBINED_PROMPT="This is a git pull request email. Please perform two tasks:

1. First, extract ONLY the portion that describes the actual code changes. Look for a bulleted list of changes that follows the pull request introduction. Do not include any other parts of the email such as commit logs, file changes, or metadata.

2. Then, rewrite the extracted text in imperative voice, being EXTREMELY careful not to omit ANY details from the original text. Your rewritten text MUST include EVERY SINGLE detail, concept, qualifier, number, and piece of information from the original.

Format your response like this:
<extracted>
... The extracted text here ...
</extracted>

<thinking>
... Your thought process for the imperative voice conversion here ...
</thinking>

<rewritten>
... The rewritten text in imperative voice here ...
</rewritten>"

	case "$PROVIDER" in
		claude)
			# Prepare request for Claude
			PR_BODY=$(cat <<EOF
{
    "model": "claude-3-sonnet-20240229",
    "max_tokens": 4000,
    "system": "You are a helpful assistant that extracts relevant changes from Git pull request emails and converts them to imperative voice.",
    "messages": [
        {
            "role": "user",
            "content": "${COMBINED_PROMPT}\n\n$(echo "$FILE_CONTENT" | jq -Rs . | sed 's/^"//;s/"$//')"
        }
    ],
    "temperature": 0.3
}
EOF
)

			PR_RESPONSE=$(curl -s -w "\n%{http_code}" https://api.anthropic.com/v1/messages \
				-H "x-api-key: $CLAUDE_API_KEY" \
				-H "anthropic-version: 2023-06-01" \
				-H "content-type: application/json" \
				-d "$PR_BODY")

			# Split response into body and status code
			PR_HTTP_BODY=$(echo "$PR_RESPONSE" | sed '$ d')
			PR_HTTP_STATUS=$(echo "$PR_RESPONSE" | tail -n1)

			# Check if the request was successful
			if [ "$PR_HTTP_STATUS" -ne 200 ]; then
				echo "Error from Claude API (Status: $PR_HTTP_STATUS):"
				echo "$PR_HTTP_BODY" | jq . 2>/dev/null || echo "$PR_HTTP_BODY"
				exit 1
			fi

			COMBINED_RESULT=$(echo "$PR_HTTP_BODY" | jq -r '.content[0].text' 2>/dev/null)
			;;

		openai)
			# Prepare request for OpenAI
			PR_BODY=$(cat <<EOF
{
    "model": "gpt-4o",
    "max_tokens": 4000,
    "temperature": 0.3,
    "messages": [
        {
            "role": "system",
            "content": "You are a helpful assistant that extracts relevant changes from Git pull request emails and converts them to imperative voice."
        },
        {
            "role": "user",
            "content": "${COMBINED_PROMPT}\n\n$(echo "$FILE_CONTENT" | jq -Rs . | sed 's/^"//;s/"$//')"
        }
    ]
}
EOF
)

			PR_RESPONSE=$(curl -s -w "\n%{http_code}" https://api.openai.com/v1/chat/completions \
				-H "Authorization: Bearer $OPENAI_API_KEY" \
				-H "Content-Type: application/json" \
				-d "$PR_BODY")

			# Split response into body and status code
			PR_HTTP_BODY=$(echo "$PR_RESPONSE" | sed '$ d')
			PR_HTTP_STATUS=$(echo "$PR_RESPONSE" | tail -n1)

			# Check if the request was successful
			if [ "$PR_HTTP_STATUS" -ne 200 ]; then
				echo "Error from OpenAI API (Status: $PR_HTTP_STATUS):"
				echo "$PR_HTTP_BODY" | jq . 2>/dev/null || echo "$PR_HTTP_BODY"
				exit 1
			fi

			COMBINED_RESULT=$(echo "$PR_HTTP_BODY" | jq -r '.choices[0].message.content' 2>/dev/null)
			;;

		huggingface)
			# Prepare request for HuggingFace
			PR_BODY=$(cat <<EOF
{
    "inputs": "System: You are a helpful assistant that extracts relevant changes from Git pull request emails and converts them to imperative voice.\n\nUser: ${COMBINED_PROMPT}\n\n$(echo "$FILE_CONTENT" | jq -Rs . | sed 's/^"//;s/"$//')\n\nAssistant:",
    "parameters": {
        "temperature": 0.3,
        "max_new_tokens": 4000,
        "return_full_text": false
    }
}
EOF
)

			PR_RESPONSE=$(curl -s -w "\n%{http_code}" https://api-inference.huggingface.co/models/meta-llama/Meta-Llama-3.1-70B-Instruct \
				-H "Authorization: Bearer $HF_API_KEY" \
				-H "Content-Type: application/json" \
				-d "$PR_BODY")

			# Split response into body and status code
			PR_HTTP_BODY=$(echo "$PR_RESPONSE" | sed '$ d')
			PR_HTTP_STATUS=$(echo "$PR_RESPONSE" | tail -n1)

			# Check if the request was successful
			if [ "$PR_HTTP_STATUS" -ne 200 ]; then
				echo "Error from HuggingFace API (Status: $PR_HTTP_STATUS):"
				echo "$PR_HTTP_BODY" | jq . 2>/dev/null || echo "$PR_HTTP_BODY"
				exit 1
			fi

			COMBINED_RESULT=$(echo "$PR_HTTP_BODY" | jq -r '.generated_text' 2>/dev/null)
			;;

		custom)
			# Use custom script with combined extraction and conversion
			COMBINED_RESULT=$("$CUSTOM_LLM" "$FILE_CONTENT" "extract_and_convert")
			;;
	esac

	# Check if extraction and conversion was successful
	if [ -z "$COMBINED_RESULT" ]; then
		echo "Error: Failed to process the pull request email."
		exit 1
	fi

	# In debug mode, display both extracted and rewritten text
	if [ "$DEBUG_MODE" = true ]; then
		# Extract the extracted text
		EXTRACTED_TEXT=$(echo "$COMBINED_RESULT" | sed -n '/<extracted>/,/<\/extracted>/p' | sed 's/<extracted>//;s/<\/extracted>//')

		echo -e "\nExtracted Changes:"
		echo "-------------------"
		echo "$EXTRACTED_TEXT"
		echo "-------------------"

		echo -e "\nImperative Voice Conversion (Combined Processing):"
		echo "-------------------"
		echo "$COMBINED_RESULT"
		echo "-------------------"
	fi

	# Extract just the rewritten text for normal mode
	REWRITTEN_TEXT=$(echo "$COMBINED_RESULT" | sed -n '/<rewritten>/,/<\/rewritten>/p' | sed 's/<rewritten>//;s/<\/rewritten>//')

	# If no rewritten text was found, use a fallback
	if [ -z "$REWRITTEN_TEXT" ]; then
		# If no rewritten text tags are found, clean and use the whole response
		CLEANED_RESPONSE=$(echo "$COMBINED_RESULT" | sed 's/<[^>]*>//g' | sed '/^$/d')
		echo "$CLEANED_RESPONSE"
	else
		# Output only the rewritten text
		echo -e "$REWRITTEN_TEXT"
	fi

	# If output file is specified, save the appropriate content
	if [ -n "$OUTPUT_FILE" ]; then
		if [ "$DEBUG_MODE" = true ]; then
			# Save the full response in debug mode
			echo "$COMBINED_RESULT" > "$OUTPUT_FILE"
			echo -e "\nOutput saved to: $OUTPUT_FILE"
		else
			# Save only the rewritten text in normal mode
			if [ -n "$REWRITTEN_TEXT" ]; then
				echo "$REWRITTEN_TEXT" > "$OUTPUT_FILE"
			else
				echo "$CLEANED_RESPONSE" > "$OUTPUT_FILE"
			fi
			echo -e "\nOutput saved to: $OUTPUT_FILE"
		fi
	fi

	exit 0  # Exit after PR processing is complete
else
	# Original flow for non-PR mode
	# Show what we're doing, but only in debug mode
	if [ "$DEBUG_MODE" = true ]; then
		echo "Checking if the text is in imperative voice using $PROVIDER..."
		echo "This may take a moment..."
	fi

	# Properly escape the file content for JSON
	# Convert the file content to a JSON-encoded string using jq
	ESCAPED_CONTENT=$(echo "$FILE_CONTENT" | jq -Rs .)

	# Function to call Claude API
	call_claude()
	{
		# Save the request to a temporary file for debugging
		REQUEST_BODY=$(cat <<EOF
{
    "model": "claude-3-sonnet-20240229",
    "max_tokens": 4000,
    "system": "You are an expert at identifying writing styles and converting text to imperative voice. Your task is to determine if the provided text is written in imperative voice. If not, rewrite it in imperative voice, being EXTREMELY careful not to omit ANY details from the original. The rewritten text must not lose any information whatsoever - every technical term, parameter, implementation detail, number and concept must be preserved. Make sure to show your thinking process by including a <thinking>...</thinking> section and wrap your rewritten text with <rewritten>...</rewritten> tags:\n\n${ESCAPED_CONTENT:1:-1}"
        }
    ],
    "temperature": 0.3
}
EOF
)

		RESPONSE=$(curl -s -w "\n%{http_code}" https://api.anthropic.com/v1/messages \
			-H "x-api-key: $CLAUDE_API_KEY" \
			-H "anthropic-version: 2023-06-01" \
			-H "content-type: application/json" \
			-d "$REQUEST_BODY")

		# Split response into body and status code
		HTTP_BODY=$(echo "$RESPONSE" | sed '$ d')
		HTTP_STATUS=$(echo "$RESPONSE" | tail -n1)

		# Check if the request was successful
		if [ "$HTTP_STATUS" -ne 200 ]; then
			echo "Error from Claude API (Status: $HTTP_STATUS):"
			echo "$HTTP_BODY" | jq . 2>/dev/null || echo "$HTTP_BODY"
			exit 1
		fi

		# Extract the assistant's response
		echo "$HTTP_BODY" | jq -r '.content[0].text' 2>/dev/null
	}

	# Function to call OpenAI API
	call_openai()
	{
		# Prepare the OpenAI API request
		REQUEST_BODY=$(cat <<EOF
{
    "model": "gpt-4o",
    "max_tokens": 4000,
    "temperature": 0.3,
    "messages": [
        {
            "role": "system",
            "content": "You are an expert at identifying writing styles and converting text to imperative voice. Your task is to determine if the provided text is written in imperative voice. If not, rewrite it in imperative voice, being EXTREMELY careful not to omit ANY details from the original text. Your rewritten text MUST include EVERY SINGLE detail, concept, qualifier, number, and piece of information from the original. This is CRITICALLY important. If the original mentions specific technical terms, parameters, options, or implementation details, ALL of these MUST be present in your rewritten version. Explain your thinking thoroughly. IMPORTANT: Before giving your final answer, think through your analysis step by step in a section labeled \"<thinking>...</thinking>\". After your thinking section, start your rewritten text with \"<rewritten>\" and end it with \"</rewritten>\"."
        },
        {
            "role": "user",
            "content": "Analyze this text and determine if it is written in imperative voice. If it is not in imperative voice, please rewrite it maintaining LITERALLY EVERY DETAIL from the original. The rewritten text must not lose any information whatsoever - every technical term, parameter, implementation detail, number and concept must be preserved. Make sure to show your thinking process by including a <thinking>...</thinking> section and wrap your rewritten text with <rewritten>...</rewritten> tags:\n\n${ESCAPED_CONTENT:1:-1}"
        }
    ]
}
EOF
)

		# Call the OpenAI API
		RESPONSE=$(curl -s -w "\n%{http_code}" https://api.openai.com/v1/chat/completions \
			-H "Authorization: Bearer $OPENAI_API_KEY" \
			-H "Content-Type: application/json" \
			-d "$REQUEST_BODY")

		# Split response into body and status code
		HTTP_BODY=$(echo "$RESPONSE" | sed '$ d')
		HTTP_STATUS=$(echo "$RESPONSE" | tail -n1)

		# Check if the request was successful
		if [ "$HTTP_STATUS" -ne 200 ]; then
			echo "Error from OpenAI API (Status: $HTTP_STATUS):"
			echo "$HTTP_BODY" | jq . 2>/dev/null || echo "$HTTP_BODY"
			exit 1
		fi

		# Extract the assistant's response
		echo "$HTTP_BODY" | jq -r '.choices[0].message.content' 2>/dev/null
	}

	# Function to call HuggingFace API
	call_huggingface()
	{
		# Prepare the HuggingFace API request - using Llama 3.1 70B Instruct
		REQUEST_BODY=$(cat <<EOF
{
    "inputs": "System: You are an expert at identifying writing styles and converting text to imperative voice. Your task is to determine if the provided text is written in imperative voice. If not, rewrite it in imperative voice, being EXTREMELY careful not to omit ANY details from the original text. Your rewritten text MUST include EVERY SINGLE detail, concept, qualifier, number, and piece of information from the original. This is CRITICALLY important. If the original mentions specific technical terms, parameters, options, or implementation details, ALL of these MUST be present in your rewritten version. Explain your thinking thoroughly. IMPORTANT: Before giving your final answer, think through your analysis step by step in a section labeled \"<thinking>...</thinking>\". After your thinking section, start your rewritten text with \"<rewritten>\" and end it with \"</rewritten>\".\n\nUser: Analyze this text and determine if it is written in imperative voice. If it is not in imperative voice, please rewrite it maintaining LITERALLY EVERY DETAIL from the original. The rewritten text must not lose any information whatsoever - every technical term, parameter, implementation detail, number and concept must be preserved. Make sure to show your thinking process by including a <thinking>...</thinking> section and wrap your rewritten text with <rewritten>...</rewritten> tags:\n\n${ESCAPED_CONTENT:1:-1}\n\nAssistant:",
    "parameters": {
        "temperature": 0.3,
        "max_new_tokens": 4000,
        "return_full_text": false
    }
}
EOF
)

		# Call the HuggingFace API using Llama 3.1 70B Instruct
		RESPONSE=$(curl -s -w "\n%{http_code}" https://api-inference.huggingface.co/models/meta-llama/Meta-Llama-3.1-70B-Instruct \
			-H "Authorization: Bearer $HF_API_KEY" \
			-H "Content-Type: application/json" \
			-d "$REQUEST_BODY")

		# Split response into body and status code
		HTTP_BODY=$(echo "$RESPONSE" | sed '$ d')
		HTTP_STATUS=$(echo "$RESPONSE" | tail -n1)

		# Check if the request was successful
		if [ "$HTTP_STATUS" -ne 200 ]; then
			echo "Error from HuggingFace API (Status: $HTTP_STATUS):"
			echo "$HTTP_BODY" | jq . 2>/dev/null || echo "$HTTP_BODY"
			exit 1
		fi

		# Extract the generated text
		echo "$HTTP_BODY" | jq -r '.generated_text' 2>/dev/null
	}

	# Function to call a custom LLM script
	call_custom_llm()
	{
		# Call the custom script and pass the escaped content
		# The custom script should handle the API call and return the response
		"$CUSTOM_LLM" "$FILE_CONTENT"

		# Check if the script execution was successful
		if [ $? -ne 0 ]; then
			echo "Error: Custom LLM script failed"
			exit 1
		fi
	}

	# Call the appropriate provider
	case "$PROVIDER" in
		claude)
			ASSISTANT_RESPONSE=$(call_claude)
			;;
		openai)
			ASSISTANT_RESPONSE=$(call_openai)
			;;
		huggingface)
			ASSISTANT_RESPONSE=$(call_huggingface)
			;;
		custom)
			ASSISTANT_RESPONSE=$(call_custom_llm)
			;;
	esac

	# Process the response based on debug mode
	if [ "$DEBUG_MODE" = true ]; then
		# Display the full response in debug mode
		echo -e "\nLLM Analysis (using $PROVIDER):"
		echo "-------------------"
		echo "$ASSISTANT_RESPONSE"
		echo "-------------------"
	else
		# Extract only the rewritten text between <rewritten> and </rewritten> tags
		REWRITTEN_TEXT=$(echo "$ASSISTANT_RESPONSE" | sed -n '/<rewritten>/,/<\/rewritten>/p' | sed 's/<rewritten>//;s/<\/rewritten>//')

		# If no rewritten text was found, check if the text was already in imperative voice
		if [ -z "$REWRITTEN_TEXT" ]; then
			if echo "$ASSISTANT_RESPONSE" | grep -q "already in imperative voice"; then
				echo -e "\nThe text is already in imperative voice. No rewriting needed."
				echo -e "\nOriginal text:"
				echo "$FILE_CONTENT"
			else
				# If no rewritten text tags are found and it's not already in imperative voice,
				# remove all tags and output as a fallback
				CLEANED_RESPONSE=$(echo "$ASSISTANT_RESPONSE" | sed 's/<[^>]*>//g' | sed '/^$/d')
				echo "$CLEANED_RESPONSE"
			fi
		else
			# Output only the rewritten text
			echo -e "$REWRITTEN_TEXT"
		fi
	fi

	# If output file is specified, save the appropriate content
	if [ -n "$OUTPUT_FILE" ]; then
		if [ "$DEBUG_MODE" = true ]; then
			# Save the full response in debug mode
			echo "$ASSISTANT_RESPONSE" > "$OUTPUT_FILE"
			echo -e "\nOutput saved to: $OUTPUT_FILE"
		else
			# Save only the rewritten text in normal mode
			if [ -n "$REWRITTEN_TEXT" ]; then
				echo "$REWRITTEN_TEXT" > "$OUTPUT_FILE"
			else
				# If no rewritten text was found, save the original content
				echo "$FILE_CONTENT" > "$OUTPUT_FILE"
			fi
		fi
	fi

	if [ "$DEBUG_MODE" = true ]; then
		echo -e "\nDone!"
	fi
fi
