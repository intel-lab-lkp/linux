.. SPDX-License-Identifier: GPL-2.0

Crypto Engine
=============

Overview
--------
The crypto engine (CE) API is a crypto queue manager.
It is the in-kernel crypto way to enqueue asynchronous crypto requests
instead of instantiating your own workqueue.

Requirement
-----------
For registration with the use of an crypto engine instance the
transformation needs to implement the respective ``struct *_engine_alg``.
For example a skcipher transformation implements
``struct skcipher_engine_alg``. This struct consists of the usual
transformation struct (for example ``struct skcipher_alg``) plus a
``struct crypto_engine_op`` which provides the callback used by the
crypto engine to run the asynchronous requests.

The transformation implements the callback function
``int (*do_one_request)(struct crypto_engine *engine, void *areq)``.
This callback is invoked by the engine to process asynchronous
requests which have been previously pushed to the engine with one of
the ``crypto_transfer_*_request_to_engine()``.
The ``do_one_request()`` implementation needs to handle the request
and on successful processing completes the request with a call to
``crypto_finalize_*_request()`` and a return value of 0. A return
value other than 0 indicates an error condition and the request is
unsuccessful marked as completed with this error value by the engine.
A special treatment is done for the return value ``-ENOSPC``. At
allocation of the engine instance via
``crypto_engine_alloc_init_and_set(..., bool retry_support, ...)``
with the ``retry_support`` parameter set to true, the engine instance
handles the ``-ENOSPC`` by re-queuing the request into the backlog and
at a later time the callback is invoked again to process this request.

Order of operations
-------------------
You are required to obtain a struct crypto_engine via ``crypto_engine_alloc_init()``.
Start it via ``crypto_engine_start()``. When finished with your work, shut down the
engine using ``crypto_engine_stop()`` and destroy the engine with
``crypto_engine_exit()``.

Before transferring any request, you may provide additional callback
functions within the ``struct engine`` instance you got from the alloc
call:

* ``prepare_crypt_hardware``: Called once before any
  ``do_one_request()`` invocations are done.

* ``unprepare_crypt_hardware``: Called once after the
  ``do_one_request()`` are done.

When your driver receives a crypto_request, and you want this request
to be processed asynchronously, you must transfer it to the crypto
engine via one of:

* crypto_transfer_aead_request_to_engine()

* crypto_transfer_akcipher_request_to_engine()

* crypto_transfer_hash_request_to_engine()

* crypto_transfer_kpp_request_to_engine()

* crypto_transfer_skcipher_request_to_engine()

At the end of the request process, a call to one of the following functions is needed:

* crypto_finalize_aead_request()

* crypto_finalize_akcipher_request()

* crypto_finalize_hash_request()

* crypto_finalize_kpp_request()

* crypto_finalize_skcipher_request()
