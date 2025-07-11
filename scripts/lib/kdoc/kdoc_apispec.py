#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""
Generate C macro invocations for kernel API specifications from kernel-doc comments.

This module creates C header files with API specification macros that match
the kernel API specification framework introduced in commit 9688de5c25bed.
"""

from kdoc_output import OutputFormat


class ApiSpecFormat(OutputFormat):
    """Generate C macro invocations for kernel API specifications"""

    def __init__(self):
        """Initialize ApiSpecFormat"""
        super().__init__()
        self.api_specs = {}

    def _format_macro_param(self, value):
        """Format a value for use in C macro parameter"""
        if value is None:
            return '""'
        # Escape quotes and backslashes for C string literals
        value = str(value).replace('\\', '\\\\').replace('"', '\\"')
        # Handle multi-line strings by replacing newlines with escaped newlines
        value = value.replace('\n', '\\n"\n\t\t       "')
        return f'"{value}"'

    def _get_api_section_value(self, sections, key):
        """Get value from API sections"""
        # sections is a dictionary where keys are section names
        # Check both with and without @ prefix
        if key in sections:
            content = sections[key]
            # Return full content, stripping trailing newlines
            return content.rstrip('\n')
        elif '@' + key in sections:
            content = sections['@' + key]
            return content.rstrip('\n')
        return None

    def _get_all_section_lines(self, sections, key):
        """Get all lines from a section"""
        if key in sections:
            return [line.strip() for line in sections[key].strip().split('\n') if line.strip()]
        elif '@' + key in sections:
            return [line.strip() for line in sections['@' + key].strip().split('\n') if line.strip()]
        return []

    def _process_error_code(self, error_lines, error_idx):
        """Process a multi-line error code specification"""
        if not error_lines:
            return

        # First line has: -ECODE, NAME, Short desc, Long desc
        first_line = error_lines[0]
        parts = first_line.split(',', 3)
        if len(parts) >= 3:
            code = parts[0].strip()
            name = parts[1].strip()
            short_desc = parts[2].strip()

            # Collect long description from remaining parts and lines
            long_desc_parts = []
            if len(parts) > 3:
                long_desc_parts.append(parts[3].strip())
            # Add continuation lines
            for line in error_lines[1:]:
                long_desc_parts.append(line.strip())
            long_desc = ' '.join(long_desc_parts)

            self.data += f"\n\tKAPI_ERROR({error_idx}, {code}, {self._format_macro_param(name)}, "
            self.data += f"{self._format_macro_param(short_desc)},\n\t\t   {self._format_macro_param(long_desc)})\n"

    def _parse_param_spec(self, param_name, sections):
        """Parse all specifications for a parameter"""
        specs = {}

        # Look for parameter-specific sections
        for key, content in sections.items():
            # Check both param- and @param- prefixes
            if key.startswith('param-') or key.startswith('@param-'):
                # Remove @ prefix if present for specs key
                specs_key = key[1:] if key.startswith('@') else key

                # Each section may contain multiple parameter specifications
                # separated by newlines
                for line in content.strip().split('\n'):
                    line = line.strip()
                    if ',' in line and line.split(',', 1)[0].strip() == param_name:
                        value = line.split(',', 1)[1].strip()
                        specs[specs_key] = value
                        break  # Found this parameter's value for this key

        return specs

    def out_function(self, fname, name, args):
        """Generate API spec for a function"""
        function_name = args.get('function', '')
        sections = args.get('sections', {})
        parameterlist = args.get('parameterlist', [])
        parameterdescs = args.get('parameterdescs', {})
        parametertypes = args.get('parametertypes', {})
        purpose = args.get('purpose', '')

        # Check if this function has an API specification
        # Look for key API spec sections that indicate this is a full API specification
        api_indicators = [
            'api-type', 'context-flags', 'param-type', 'error-code',
            'capability', 'signal', 'lock', 'side-effect', 'state-trans'
        ]

        has_api_spec = False
        for indicator in api_indicators:
            # Check both with and without @ prefix
            if any(key.startswith(indicator) or key.startswith('@' + indicator) for key in sections.keys()):
                has_api_spec = True
                break

        if not has_api_spec:
            return

        # Clear warnings for API spec output since parameter-specific sections
        # trigger false warnings
        args['warnings'] = []


        # Start building the macro invocation
        self.data += f"DEFINE_KERNEL_API_SPEC({function_name})\n"

        # Add description
        if purpose:
            self.data += f"\tKAPI_DESCRIPTION({self._format_macro_param(purpose)})\n"

        # Add long description if present
        long_desc = self._get_api_section_value(sections, 'long-desc')
        if long_desc:
            self.data += f"\tKAPI_LONG_DESC({self._format_macro_param(long_desc)})\n"

        # Add context flags
        context_flags = self._get_api_section_value(sections, 'context-flags')
        if context_flags:
            self.data += f"\tKAPI_CONTEXT({context_flags})\n"
        elif self._get_api_section_value(sections, 'context'):
            # Fallback to simple context
            self.data += f"\tKAPI_CONTEXT({self._get_api_section_value(sections, 'context')})\n"

        # Add parameter count first
        param_count = len(parameterlist)
        param_count_val = self._get_api_section_value(sections, 'param-count')
        # Note: KAPI_PARAM_COUNT doesn't exist in the current infrastructure
        # Parameters are handled individually with KAPI_PARAM/KAPI_PARAM_END

        # Process parameters
        for param_idx, param in enumerate(parameterlist):
            param_name = param.strip()
            param_desc = parameterdescs.get(param_name, '')
            param_ctype = parametertypes.get(param_name, '')

            # Get all parameter specifications
            param_specs = self._parse_param_spec(param_name, sections)

            self.data += f"\n\tKAPI_PARAM({param_idx}, {self._format_macro_param(param_name)}, "
            self.data += f"{self._format_macro_param(param_ctype)}, {self._format_macro_param(param_desc)})\n"

            # Add parameter type
            if 'param-type' in param_specs:
                self.data += f"\t\tKAPI_PARAM_TYPE({param_specs['param-type']})\n"

            # Add parameter flags
            if 'param-flags' in param_specs:
                self.data += f"\t\tKAPI_PARAM_FLAGS({param_specs['param-flags']})\n"

            # Add constraint type
            if 'param-constraint-type' in param_specs:
                self.data += f"\t\tKAPI_PARAM_CONSTRAINT_TYPE({param_specs['param-constraint-type']})\n"

            # Add range
            if 'param-range' in param_specs:
                if ',' in param_specs['param-range']:
                    min_val, max_val = param_specs['param-range'].split(',', 1)
                    self.data += f"\t\tKAPI_PARAM_RANGE({min_val.strip()}, {max_val.strip()})\n"

            # Add mask
            if 'param-mask' in param_specs:
                self.data += f"\t\tKAPI_PARAM_MASK({param_specs['param-mask']})\n"

            # Add constraint description
            if 'param-constraint' in param_specs:
                self.data += f"\t\tKAPI_PARAM_CONSTRAINT({self._format_macro_param(param_specs['param-constraint'])})\n"

            # struct-type information is stored as comments for documentation purposes
            # The actual struct validation happens in the kernel based on param type

            self.data += "\tKAPI_PARAM_END\n"

        # Add return specification if we have meaningful return information
        return_type = self._get_api_section_value(sections, 'return-type')
        return_check = self._get_api_section_value(sections, 'return-check-type') or \
                      self._get_api_section_value(sections, 'return-check')
        return_success = self._get_api_section_value(sections, 'return-success')

        if return_type or return_check or return_success:
            # Get return description but don't use generic ones
            return_desc = sections.get('return', sections.get('returns', sections.get('Return', '')))
            if return_desc and any(phrase in return_desc.lower() for phrase in
                                 ['error code', 'negative error', 'success or error']):
                return_desc = ""  # Skip generic descriptions

            self.data += f"\n\tKAPI_RETURN({self._format_macro_param(parametertypes.get('', 'long'))}, "
            self.data += f"{self._format_macro_param(return_desc)})\n"

            if return_type:
                self.data += f"\t\tKAPI_RETURN_TYPE({return_type})\n"

            if return_check:
                self.data += f"\t\tKAPI_RETURN_CHECK_TYPE({return_check})\n"

            if return_success:
                self.data += f"\t\tKAPI_RETURN_SUCCESS({return_success})\n"

            self.data += "\tKAPI_RETURN_END\n"

        # Add error count before processing errors
        error_lines = self._get_all_section_lines(sections, 'error-code')
        error_count = self._get_api_section_value(sections, 'error-count')
        if error_count:
            self.data += f"\n\tKAPI_RETURN_ERROR_COUNT({error_count})\n"
        else:
            # Count the error lines
            error_line_count = 0
            for line in error_lines:
                if line.startswith('-'):
                    error_line_count += 1
            if error_line_count > 0:
                self.data += f"\n\tKAPI_RETURN_ERROR_COUNT({error_line_count})\n"

        # Process error codes with extended format
        error_idx = 0

        # Process each error-code entry (which may span multiple lines)
        current_error = []
        for line in error_lines:
            # Check if this starts a new error code (starts with -)
            if line.startswith('-') and current_error:
                # Process the previous error
                self._process_error_code(current_error, error_idx)
                error_idx += 1
                current_error = [line]
            else:
                # Continuation of current error
                current_error.append(line)

        # Process the last error
        if current_error:
            self._process_error_code(current_error, error_idx)
            error_idx += 1

        # Add lock count before processing locks
        lock_lines = self._get_all_section_lines(sections, 'lock')
        lock_count = self._get_api_section_value(sections, 'lock-count')
        if lock_count:
            self.data += f"\n\tKAPI_LOCK_COUNT({lock_count})\n"
        else:
            # Count lock lines
            lock_line_count = len(lock_lines)
            lock_req = self._get_api_section_value(sections, 'lock-req')
            if lock_req and lock_line_count == 0:
                lock_line_count = 1
            if lock_line_count > 0:
                self.data += f"\n\tKAPI_LOCK_COUNT({lock_line_count})\n"

        # Process locks
        lock_idx = 0
        for line in lock_lines:
            parts = line.split(',')
            if len(parts) >= 2:
                lock_name = parts[0].strip()
                lock_type = parts[1].strip()
                self.data += f"\n\tKAPI_LOCK({lock_idx}, {self._format_macro_param(lock_name)}, {lock_type})\n"

                # Check for lock attributes
                if self._get_api_section_value(sections, 'lock-acquired'):
                    self.data += "\t\tKAPI_LOCK_ACQUIRED\n"
                if self._get_api_section_value(sections, 'lock-released'):
                    self.data += "\t\tKAPI_LOCK_RELEASED\n"

                lock_desc = self._get_api_section_value(sections, 'lock-desc')
                if lock_desc:
                    self.data += f"\t\tKAPI_LOCK_DESC({self._format_macro_param(lock_desc)})\n"

                self.data += "\tKAPI_LOCK_END\n"
                lock_idx += 1

        # Legacy lock-req support
        lock_req = self._get_api_section_value(sections, 'lock-req')
        if lock_req and lock_idx == 0:
            self.data += f"\n\tKAPI_LOCK(0, {self._format_macro_param(lock_req)}, KAPI_LOCK_CUSTOM)\n"
            self.data += f"\t\tKAPI_LOCK_DESC({self._format_macro_param(lock_req)})\n"
            self.data += "\tKAPI_LOCK_END\n"
            lock_idx = 1

        # Add constraint count before processing constraints
        constraint_lines = self._get_all_section_lines(sections, 'constraint')
        const_count = self._get_api_section_value(sections, 'constraint-count')
        if const_count:
            self.data += f"\n\tKAPI_CONSTRAINT_COUNT({const_count})\n"
        elif len(constraint_lines) > 0:
            self.data += f"\n\tKAPI_CONSTRAINT_COUNT({len(constraint_lines)})\n"

        # Process constraints first (before signals/capabilities/etc)
        constraint_idx = 0
        for line in constraint_lines:
            parts = line.split(',', 1)
            if parts:
                name = parts[0].strip()
                desc = parts[1].strip() if len(parts) > 1 else ""

                self.data += f"\n\tKAPI_CONSTRAINT({constraint_idx}, {self._format_macro_param(name)},\n"
                self.data += f"\t\t\t{self._format_macro_param(desc)})\n"

                # Check for constraint expression
                expr_lines = self._get_all_section_lines(sections, 'constraint-expr')
                for expr_line in expr_lines:
                    if expr_line.startswith(name):
                        expr = expr_line.split(',', 1)[1].strip() if ',' in expr_line else ""
                        self.data += f"\t\tKAPI_CONSTRAINT_EXPR({self._format_macro_param(expr)})\n"
                        break

                self.data += "\tKAPI_CONSTRAINT_END\n"
                constraint_idx += 1

        # Process signals
        signal_idx = 0
        signal_lines = self._get_all_section_lines(sections, 'signal')
        signal_count = self._get_api_section_value(sections, 'signal-count')

        if signal_count:
            self.data += f"\n\tKAPI_SIGNAL_COUNT({signal_count})\n"
        elif len(signal_lines) > 0:
            self.data += f"\n\tKAPI_SIGNAL_COUNT({len(signal_lines)})\n"

        for line in signal_lines:
            # Remove this redundant signal count check

            self.data += f"\n\tKAPI_SIGNAL({signal_idx}, 0, {self._format_macro_param(line)}, "

            # Add signal direction
            signal_dir = self._get_api_section_value(sections, 'signal-direction')
            if signal_dir:
                self.data += f"{signal_dir}, "
            else:
                self.data += "KAPI_SIGNAL_RECEIVE, "

            # Add signal action
            signal_action = self._get_api_section_value(sections, 'signal-action')
            if signal_action:
                self.data += f"{signal_action})\n"
            else:
                self.data += "KAPI_SIGNAL_ACTION_RETURN)\n"

            # Add signal attributes
            signal_cond = self._get_api_section_value(sections, 'signal-condition')
            if signal_cond:
                self.data += f"\t\tKAPI_SIGNAL_CONDITION({self._format_macro_param(signal_cond)})\n"

            signal_desc = self._get_api_section_value(sections, 'signal-desc')
            if signal_desc:
                self.data += f"\t\tKAPI_SIGNAL_DESC({self._format_macro_param(signal_desc)})\n"

            signal_error = self._get_api_section_value(sections, 'signal-error')
            if signal_error:
                self.data += f"\t\tKAPI_SIGNAL_ERROR({signal_error})\n"

            signal_timing = self._get_api_section_value(sections, 'signal-timing')
            if signal_timing:
                self.data += f"\t\tKAPI_SIGNAL_TIMING({signal_timing})\n"

            signal_priority = self._get_api_section_value(sections, 'signal-priority')
            if signal_priority:
                self.data += f"\t\tKAPI_SIGNAL_PRIORITY({signal_priority})\n"

            if self._get_api_section_value(sections, 'signal-interruptible'):
                self.data += "\t\tKAPI_SIGNAL_INTERRUPTIBLE\n"

            signal_state = self._get_api_section_value(sections, 'signal-state-req')
            if signal_state:
                self.data += f"\t\tKAPI_SIGNAL_STATE_REQ({signal_state})\n"

            self.data += "\tKAPI_SIGNAL_END\n"
            signal_idx += 1

        # Process side effects
        side_effect_lines = self._get_all_section_lines(sections, 'side-effect')
        effect_count = self._get_api_section_value(sections, 'side-effect-count')
        if effect_count:
            self.data += f"\n\tKAPI_SIDE_EFFECT_COUNT({effect_count})\n"
        elif len(side_effect_lines) > 0:
            self.data += f"\n\tKAPI_SIDE_EFFECT_COUNT({len(side_effect_lines)})\n"

        # Actually process side effects
        side_effect_idx = 0
        for line in side_effect_lines:
            # Parse: type, target, description[, key=value pairs]
            # First extract any key=value pairs at the end
            import re

            # Extract condition=... and reversible=yes
            condition = None
            reversible = False

            # Look for condition=... pattern
            cond_match = re.search(r',\s*condition=([^,]+?)(?:\s*,\s*reversible=yes\s*)?$', line)
            if cond_match:
                condition = cond_match.group(1).strip()
                line = line[:cond_match.start()]  # Remove condition from line

            # Check for reversible=yes
            if line.endswith(', reversible=yes'):
                reversible = True
                line = line[:-len(', reversible=yes')]
            elif ', reversible=yes,' in line:
                reversible = True
                line = line.replace(', reversible=yes,', ',')

            # Now parse the main parts
            parts = line.split(',', 2)
            if len(parts) >= 2:
                effect_type = parts[0].strip()
                target = parts[1].strip()
                desc = parts[2].strip() if len(parts) > 2 else ""

                self.data += f"\n\tKAPI_SIDE_EFFECT({side_effect_idx}, {effect_type},\n"
                self.data += f"\t\t\t {self._format_macro_param(target)},\n"
                self.data += f"\t\t\t {self._format_macro_param(desc)})\n"

                if condition:
                    self.data += f"\t\tKAPI_EFFECT_CONDITION({self._format_macro_param(condition)})\n"

                if reversible:
                    self.data += "\t\tKAPI_EFFECT_REVERSIBLE\n"

                self.data += "\tKAPI_SIDE_EFFECT_END\n"
                side_effect_idx += 1

        # Process state transitions
        state_trans_lines = self._get_all_section_lines(sections, 'state-trans')
        trans_count = self._get_api_section_value(sections, 'state-trans-count')
        if trans_count:
            self.data += f"\n\tKAPI_STATE_TRANS_COUNT({trans_count})\n"
        elif len(state_trans_lines) > 0:
            self.data += f"\n\tKAPI_STATE_TRANS_COUNT({len(state_trans_lines)})\n"

        state_trans_idx = 0
        for line in state_trans_lines:
            parts = line.split(',', 3)
            if len(parts) >= 3:
                target = parts[0].strip()
                from_state = parts[1].strip()
                to_state = parts[2].strip()
                desc = parts[3].strip() if len(parts) > 3 else ""

                self.data += f"\n\tKAPI_STATE_TRANS({state_trans_idx}, {self._format_macro_param(target)}, "
                self.data += f"{self._format_macro_param(from_state)}, {self._format_macro_param(to_state)},\n"
                self.data += f"\t\t\t {self._format_macro_param(desc)})\n"
                self.data += "\tKAPI_STATE_TRANS_END\n"
                state_trans_idx += 1

        # Process capabilities
        cap_lines = self._get_all_section_lines(sections, 'capability')
        cap_count = self._get_api_section_value(sections, 'capability-count')
        if cap_count:
            self.data += f"\n\tKAPI_CAPABILITY_COUNT({cap_count})\n"
        elif len(cap_lines) > 0:
            self.data += f"\n\tKAPI_CAPABILITY_COUNT({len(cap_lines)})\n"

        cap_idx = 0
        for line in cap_lines:
            parts = line.split(',', 2)
            if len(parts) >= 2:
                cap_name = parts[0].strip()
                cap_type = parts[1].strip()
                cap_desc = parts[2].strip() if len(parts) > 2 else cap_name

                self.data += f"\n\tKAPI_CAPABILITY({cap_idx}, {cap_name}, {self._format_macro_param(cap_desc)}, {cap_type})\n"

                # Check for capability attributes
                cap_allows = self._get_api_section_value(sections, 'capability-allows')
                if cap_allows:
                    self.data += f"\t\tKAPI_CAP_ALLOWS({self._format_macro_param(cap_allows)})\n"

                cap_without = self._get_api_section_value(sections, 'capability-without')
                if cap_without:
                    self.data += f"\t\tKAPI_CAP_WITHOUT({self._format_macro_param(cap_without)})\n"

                cap_cond = self._get_api_section_value(sections, 'capability-condition')
                if cap_cond:
                    self.data += f"\t\tKAPI_CAP_CONDITION({self._format_macro_param(cap_cond)})\n"

                cap_priority = self._get_api_section_value(sections, 'capability-priority')
                if cap_priority:
                    self.data += f"\t\tKAPI_CAP_PRIORITY({cap_priority})\n"

                self.data += "\tKAPI_CAPABILITY_END\n"
                cap_idx += 1

        # Add examples
        examples = self._get_api_section_value(sections, 'examples')
        if examples:
            self.data += f"\n\tKAPI_EXAMPLES({self._format_macro_param(examples)})\n"

        # Add notes
        notes = self._get_api_section_value(sections, 'notes')
        if notes:
            self.data += f"\tKAPI_NOTES({self._format_macro_param(notes)})\n"

        # Process struct specifications
        struct_types = {}
        # Find all unique struct types from struct-type and struct-field sections
        struct_type_lines = self._get_all_section_lines(sections, 'struct-type')
        for line in struct_type_lines:
            parts = line.split(',', 1)
            if len(parts) >= 2:
                struct_name = parts[1].strip()
                if struct_name not in struct_types:
                    struct_types[struct_name] = {'fields': []}

        # Collect struct fields
        struct_field_lines = self._get_all_section_lines(sections, 'struct-field')
        current_struct = None
        if struct_types:
            # Get the first struct type as the current one
            current_struct = list(struct_types.keys())[0]

        for line in struct_field_lines:
            parts = line.split(',', 2)
            if len(parts) >= 3:
                field_name = parts[0].strip()
                field_type = parts[1].strip()
                field_desc = parts[2].strip()
                if current_struct and current_struct in struct_types:
                    struct_types[current_struct]['fields'].append({
                        'name': field_name,
                        'type': field_type,
                        'desc': field_desc
                    })

        # Generate struct specifications
        if struct_types:
            struct_count = len(struct_types)
            self.data += f"\n\tKAPI_STRUCT_SPEC_COUNT({struct_count})\n"

            struct_idx = 0
            for struct_name, struct_info in struct_types.items():
                self.data += f"\n\tKAPI_STRUCT_SPEC({struct_idx}, {self._format_macro_param(struct_name)}, "
                self.data += f"{self._format_macro_param(f'Structure specification for {struct_name}')})\n"

                # Add field count
                field_count = len(struct_info['fields'])
                if field_count > 0:
                    self.data += f"\t\tKAPI_STRUCT_FIELD_COUNT({field_count})\n"

                # Add fields
                field_idx = 0
                for field in struct_info['fields']:
                    # Map common C types to KAPI types
                    kapi_type = "KAPI_TYPE_CUSTOM"
                    if field['type'] in ['__u32', '__u64', '__s32', '__s64', 'u32', 'u64', 's32', 's64']:
                        if field['type'].startswith('__s') or field['type'].startswith('s'):
                            kapi_type = "KAPI_TYPE_INT"
                        else:
                            kapi_type = "KAPI_TYPE_UINT"

                    self.data += f"\n\t\tKAPI_STRUCT_FIELD({field_idx}, {self._format_macro_param(field['name'])}, "
                    self.data += f"{kapi_type}, {self._format_macro_param(field['type'])}, "
                    self.data += f"{self._format_macro_param(field['desc'])})\n"

                    # Add field constraints from other sections
                    field_range_lines = self._get_all_section_lines(sections, 'struct-field-range')
                    for range_line in field_range_lines:
                        if range_line.startswith(field['name'] + ','):
                            range_parts = range_line.split(',')
                            if len(range_parts) >= 3:
                                self.data += f"\t\t\tKAPI_FIELD_CONSTRAINT_RANGE({range_parts[1].strip()}, {range_parts[2].strip()})\n"

                    # Add enum constraints if defined
                    field_enum_lines = self._get_all_section_lines(sections, 'struct-field-enum')
                    for enum_line in field_enum_lines:
                        if enum_line.startswith(field['name'] + ','):
                            enum_parts = enum_line.split(',', 1)
                            if len(enum_parts) >= 2:
                                self.data += f"\t\t\tKAPI_FIELD_CONSTRAINT_ENUM({self._format_macro_param(enum_parts[1].strip())})\n"

                    self.data += "\t\tKAPI_STRUCT_FIELD_END\n"
                    field_idx += 1

                self.data += "\tKAPI_STRUCT_SPEC_END\n"
                struct_idx += 1


        # Version information is not supported in the current KAPI infrastructure
        # The 'since' and 'since-version' sections are ignored for now

        self.data += "\nKAPI_END_SPEC;\n\n"

    def out_enum(self, fname, name, args):
        """Skip enum output for API specs"""
        pass

    def out_typedef(self, fname, name, args):
        """Skip typedef output for API specs"""
        pass

    def out_struct(self, fname, name, args):
        """Skip struct output for API specs"""
        pass

    def out_doc(self, fname, name, args):
        """Skip DOC block output for API specs"""
        pass