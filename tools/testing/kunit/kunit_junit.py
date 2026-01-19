# SPDX-License-Identifier: GPL-2.0
#
# Generates JSON from KUnit results according to
# KernelCI spec: https://github.com/kernelci/kernelci-doc/wiki/Test-API
#
# Copyright (C) 2025, Google LLC.
# Author: David Gow <davidgow@google.com>


from kunit_parser import Test, TestStatus

def escape_xml_string(string : str) -> str:
	return string.replace("&", "&amp;").replace("\"", "&quot;").replace("'", "&apos;").replace("<", "&lt;").replace(">", "&gt;")

def get_test_suite(test: Test) -> str:
	xml_output = '<testsuite name="' + escape_xml_string(test.name) + '" tests="' + str(test.counts.total()) + '" failures="' + str(test.counts.failed) + '" skipped="' +str(test.counts.skipped) + '">\n'

	for subtest in test.subtests:
		if subtest.subtests:
			xml_output += get_test_suite(subtest)
			continue
		xml_output += '<testcase name="' + escape_xml_string(subtest.name) + '" >\n'
		if subtest.status == TestStatus.FAILURE:
			xml_output += '<failure>Test Failed</failure>\n'
		xml_output += '<system-out><![CDATA[' + "\n".join(subtest.log) + ']]></system-out>\n'
		xml_output += '</testcase>\n'

	xml_output += '</testsuite>\n\n'

	return xml_output

def get_junit_result(test: Test) -> str:
	xml_output = '<?xml version="1.0" encoding="UTF-8" ?>\n\n'

	xml_output += get_test_suite(test)
	return xml_output
