#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only


def not_implemented(func):
    """
    Decorator to mark functions as not yet implemented.

    This decorator wraps a function and raises a NotImplementedError when the
    function is called, rather than executing the function body. This is useful
    for defining interface methods or placeholder functions that need to be
    implemented later.

    Args:
        func: The function to be wrapped.

    Returns:
        A wrapper function that raises NotImplementedError when called.

    Raises:
        NotImplementedError: Always raised when the decorated function is called.
            The exception includes the function name and any arguments that were
            passed to the function.

    Example:
        @not_implemented
        def future_feature(arg1, arg2):
            pass

        # Calling future_feature will raise:
        # NotImplementedError('future_feature', arg1_value, arg2_value)
    """
    def inner(*args, **kwargs):
        raise NotImplementedError(func.__name__, *args, **kwargs)

    return inner
