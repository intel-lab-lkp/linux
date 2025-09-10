.. title:: How-to write testable code specifications

=========================================
How-to write testable code specifications
=========================================

Introduction
------------
The Documentation/doc-guide/kernel-doc.rst chapter describes how to document the code using the kernel-doc format, however it does not specify the criteria to be followed for writing testable specifications; i.e. specifications that can be used to for the semantic description of low level requirements.

This chapter defines criteria to formally describe developers’ intent at the function and subfunction level in the form of testable expectations.

A Virtuous Cycle
----------------
By adding testable specifications at the function or (where relevant) subfunction level, one enables the creation of a virtuous cycle when testing is supplemented with open source code coverage tools like llvm-cov or Gcov.

As a true reflection of developer intent, code specifications inform the creation of a pass/fail tests which can then be assessed in conjunction with code coverage tools. A failing test may indicate broken code or specifications that fail to capture developer intent. A gap in code coverage may indicate missing specifications, unintended functionalities, or insufficient test procedure.

High level goals
----------------
The code specifications:

1. Should be maintainable together with the code.
2. Should support hierarchical traceability to allow refinement of SW dependencies (i.e. cross reference critical APIs or data structures).
3. Should describe error conditions and success behaviors.
4. Should describe conditions to be met by the user to avoid unspecified or unwanted behaviours.
5. Should allow covering both static and dynamic aspects of the code.
6. Should be compatible with Documentation/doc-guide/kernel-doc.rst.
7. Should support the definition of a test plan (i.e. syntax should enforce testability as well as the avoidance of untestable specifications, e.g “function_xyz() shall not do something”).

Format and Syntax
-----------------
Testable code specifications must be written according to the syntax already defined in Documentation/doc-guide/kernel-doc.rst with additional rules that are described below.

Function name
~~~~~~~~~~~~~
``* function_name() - Brief description of function.``

This field is to be considered informative and is not part of the testable specifications.

Input Arguments
~~~~~~~~~~~~~~~
Input arguments should be specified in a way that better supports the function’s expectations and Assumptions of Use described below.
They must not contradict the function's expectations and the function’s prototype. For example::

 * trace_set_clr_event - enable or disable an event
 * @system: system name to match (NULL for any system)
 * @event: event name to match (NULL for all events, within system)
 * @set: 1 to enable, 0 to disable
 *
 [...]
 *
 */
 int trace_set_clr_event(const char *system, const char *event, int set)

Above all the parameters clearly introduce the impact that they have on the code specifications.

However if below we had::

 * trace_set_clr_event - enable or disable an event
 * @system: system name to match (NULL for any system)
 * @event: event name to match (NULL for all events, within system)
 * @set: true to enable, false to disable \
 [...]
 */
 int trace_set_clr_event(const char *system, const char *event, int set)

In this case @set would be a bad definition since it is defined as an integer and not as a boolean.

Longer Description
~~~~~~~~~~~~~~~~~~
The `Longer Description` section is where the large part of testable code specifications are defined. The section must be organised as follows::

 * (Summary Description) provides an introduction of the functionalities
 * provided by the function and any informal note. This text does not
 * represent any testable code specification.
 *
 *
 * Function's expectations:
 * [ID1] - [code expectation]
 *
 * [ID2] - [code expectation]
 *
 * [...]
 *
 * [IDn] - [code expectation]
 *
 * Assumptions of Use:
 * [ID1] - [constraint to be met by the caller]
 *
 * [ID2] - [constraint to be met by the caller]
 *
 * [IDn] - [constraint to be met by the caller]
 *

When writing the above section the following rules must be followed:

* No rules apply to the text above ``Function’s expectations``; such a text does not constitute testable specifications and it is just informative;
* Both ``Function’s expectations`` and ``Assumptions of Use`` must be listed prefixing each of them with an ID that is unique within this kernel-doc header. The reason for this is to facilitate cross-referencing and traceability between tests and code specifications.
* A Function’s expectation is a testable behavior that the function is expected to comply with (i.e. the function is expected to behave as defined in the function’s expectation).
* An Assumption of Use is  a pre-condition to be met when invoking the function being documented.
* Testable functional expectations and Assumptions of Use must be constructed according the same rules that apply when writing software requirements:
    * Statements should include a subject and a verb, together with other elements necessary to adequately express the information content of the specifications.
    * The verbs are required to use the following keywords:
        * For mandatory expectations the verb ‘shall’ is to be used;
        * For descriptive text that do not constitute a testable expectation verbs such as ‘are’, ‘is’, ‘was’ are to be used;
        * Negative expectations must be avoided (e.g. ‘shall not’ must be avoided).
* Statements must be constructed according to the following scheme:

    [**Condition**] [**Subject**] [**Verb/Action**] [**Object**] [**Constraint of Action**].

    In this regard [**Condition**] and [**Constraint of Action**] could be omitted respectively if the [**Action**] being specified must always happen or if there are no constraints associated with it.

Function Context
~~~~~~~~~~~~~~~~
The function’s context represents an integral part of Function’s expectations and Assumptions of Use, where these can further specify the information contained in this section.

Without further specifications this section is to be interpreted as per example below:

``* Context: Any context.``

The function shall execute in any possible context.

``* Context: Any context. Takes and releases the RCU lock.``

The function shall execute in any possible context.
The function shall take and release the RCU lock.

``* Context: Any context. Expects <lock> to be held by caller.``

The function shall execute in any possible context.
<lock> is assumed to be held before this function is called.

``* Context: Process context. May sleep if @gfp flags permit.``

The function shall execute in process context.
The function shall sleep according to @gfp flags definitions

``* Context: Process context. Takes and releases <mutex>.``

The function shall execute in process context.
The function shall take and release <mutex>.

``* Context: Softirq or process context. Takes and releases <lock>, BH-safe.``

The function shall execute in process or Softirq context.
The function shall take and release <lock>.
The function shall safely execute in bottom half contexts.

``* Context: Interrupt context.``

The function shall execute in interrupt context only.

It is a good practice to further specify the context specifications as part of the Function’s expectation (e.g. at which stage a lock is held and released)

Return values
~~~~~~~~~~~~~
Return values must be written as a multiple line list in the following format::

* Return:
* * [value-1] - [condition-1]
* * [value-2] - [condition-2]
* * [...]
* * [value-n] - [condition-n]
* * Any value returned by func-1(), func-2(),...,func-n()

In such a format ``[value-i]`` must be a clearly identified value or range of values that is compatible with the function prototype (e.g. for a read() file operation, it is ok to define [value-i] as ``the number of bytes successfully copied to the user space buffer``).

``[condition-i]`` must be a condition that can be unambiguously traced back to the ``Function’s expectations`` or ``Context`` defined above; as part of [condition-i] it is possible to refer to dependencies of invoked functions or of internal SW or HW states.

``Any value returned by func-1(), func-2(),...,func-n()`` defines a scenario where the current function is directly returning the value of an invoked function dependency.

Semantic aspects of testable specifications
-------------------------------------------
From a semantic point of view it is important to document the intended or expected behavior (from a developer or integrator point of view respectively) in consideration of the different design aspects impacting it.

Such behavior shall be described in a way that makes it possible to define test cases unambiguously. \
To this extent it is important to document design elements impacting the expected behavior and the design elements characterizing the expected behavior (and sometimes these can physically overlap); such design elements shall be limited to the scope of the code being documented, that can range from a single function to multiple ones depending on the complexity of the overall code.

**Possible elements impacting the expected behavior** of the code being documented are:

* Input parameters: parameters passed to the API being documented;
* state variables: global and static data (variables or pointers);
* software dependencies: external SW APIs invoked by the code under analysis;
* Hardware dependencies: HW design elements directly impacting the behavior of the code in scope;
* Firmware dependencies: FW design elements that have an impact on the behavior of the API being documented (e.g. DTB or ACPI tables, or runtime services like SCMI and ACPI AML);
* Compile time configuration parameters: configuration parameters parsed when compiling the Kernel Image;
* Runtime configuration parameters (AKA calibration parameters): parameters that can be modified at runtime.

**Design elements characterizing the expected behavior** of the API being documented that are in scope according to the above mentioned granularity:

* API return values, including pointer addresses;
* Input pointers: pointers passed as input parameter to the API being documented;
* state variables: global and static data (variable or pointers);
* Hardware design elements (e.g. HW registers).

**Testability considerations**: the impact of each of the documented “design elements impacting the expected behavior” must be described in terms of effect on the “design element characterizing the expected behavior” and, in doing so, it is important to document allowed or not allowed ranges of values, corner cases and error conditions;  so that it is possible to define a meaningful test plan according to different equivalence classes.

**Scalability and maintainability considerations**: the described expected behavior must be limited to the scope of the code under analysis so for example the Software, Firmware and Hardware dependencies shall be described in terms of possible impact on the invoking code deferring further details to the respective documentation of these.

When deciding the scope of the code being documented, the scalability and maintainability goals must be considered; it does not make sense to embed the documentation of multiple complex functions within the kernel-doc header of the top level function as, doing so, would make it harder to review the code changes against the documented specifications and/or to extend the specifications to new functionalities being added.

The end goal is to build a hierarchical, scalable, maintainable documentation.

**Feasibility considerations**: Only the “meaningful” and “useful” expected behavior, and the design elements impacting it, shall be considered (e.g. a printk() logging some info may be omitted). There are two reasons behind this point:

1. Specifying the expected behaviour of the code should be done, in principle, in a code agnostic way. So it is not about writing a pseudo-code redundant implementation, but rather about defining and documenting the developer intent and the integrator’s expectations.
2. When the expected behavior is defined before implementing the code, such an activity is done by experts using a level of detail that is more abstract than the code itself and they only refer to aspects that are relevant for the design expectations.
