# SPDX-License-Identifier: GPL-2.0
#
# Generates JUnit XML files from KUnit test results
#
# Copyright (C) 2026, Google LLC and David Gow.

from xml.sax.saxutils import quoteattr
from kunit_parser import Test, TestStatus

# Get a string representing a tes suite (including subtests) in JUnit XML
def get_test_suite(test: Test) -> str:
	xml_output = '<testsuite name=' + quoteattr(test.name) + ' tests="' +\
		str(test.counts.total()) + '" failures="' + str(test.counts.failed) +\
		'" skipped="' + str(test.counts.skipped) + '" errors="' + str(test.counts.crashed + test.counts.errors) + '">\n'

	for subtest in test.subtests:
		if subtest.subtests:
			xml_output += get_test_suite(subtest)
			continue
		xml_output += '<testcase name=' + quoteattr(subtest.name) + '>\n'
		if subtest.status == TestStatus.FAILURE:
			xml_output += '<failure>Test Failed</failure>\n'
		elif subtest.status == TestStatus.SKIPPED:
			xml_output += '<skipped>Test Skipped</skipped>\n'
		elif subtest.status == TestStatus.TEST_CRASHED:
			xml_output += '<error>Test Crashed</error>\n'

		if subtest.log:
			xml_output +=\
				'<system-out><![CDATA[' + "\n".join(subtest.log) +  ']]></system-out>\n'

		xml_output += '</testcase>\n'

	xml_output += '</testsuite>\n\n'

	return xml_output

# Get a string for an entire XML file for the test structure starting at test
def get_junit_result(test: Test) -> str:
	xml_output = '<?xml version="1.0" encoding="UTF-8" ?>\n\n'

	xml_output += get_test_suite(test)
	return xml_output
