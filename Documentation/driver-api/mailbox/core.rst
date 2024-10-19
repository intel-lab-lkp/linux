=====================
mailbox documentation
=====================

Hardware Introduction
=====================

Mailbox hardware is a specialized component found in multi-core
processors and embedded systems that facilitates inter-processor
communication (IPC) or communication between different hardware
components. It provides a structured mechanism for sending and
receiving messages, allowing various processors or devices to
exchange data efficiently. Here's an overview of its key
characteristics and functions:

Key Characteristics of Mailbox Hardware
Interrupt Handling: Many mailbox implementations support
interrupt-driven communication. This allows a receiving processor
to be alerted when a new message arrives, facilitating immediate
processing without polling the mailbox constantly.

Hardware Registers: Mailbox hardware often includes registers for
configuration and status monitoring. These registers can be used
to control the mailbox's behavior, check for available messages,
or acknowledge message receipt.

Support for Multiple Protocols: Mailboxes can support various
communication protocols, enabling interoperability between different
hardware components and simplifying the integration of diverse systems.

Synchronous and Asynchronous Modes: Mailbox hardware can operate in
both synchronous and asynchronous modes. In synchronous mode, the
sender may wait for the receiver to acknowledge receipt before
proceeding, while in asynchronous mode, the sender can continue
executing other tasks immediately after sending the message.


Mailbox framework design
========================

The mailbox facilitates interprocessor communication by allowing processors to
exchange messages or signals. The mailbox framework consists of:

Mailbox Controller: This is platform-specific and is responsible for configuring
and managing interrupts from the remote processor. It offers a generic API for
the mailbox client.

Mailbox Client: This component handles the sending and receiving of messages.


............................................................................
:  client driver      client_a            client_b                         :
............................................................................
                            ^-------------------^
                                    |
                                    |
............................................................................
:  controller framework          mailbox                                   :
....................................|.......................................
                                    |
                                    |
............................................................................
:  controller driver          device specific                              :
....................................|.......................................
                                    |
                                    |
kernel                              |
............................................................................
hardware                            |
                                    |
                                    |
............................................................................
:                             remote processor                             :
............................................................................


In the context of a mailbox framework, a channel refers to a dedicated
communication pathway between two or more processors or components. By using
channels, the framework abstracts the complexity of interprocessor communication.

Data Structures
================

- **struct mbox_client**
  This structure represents a client that communicates over a mailbox
  channel. It holds information such as:
  - A pointer to the device associated with the client (`dev`).
  - Callback functions for handling message transmission events, including:
    - `rx_callback`: Called when a message is received.
    - `tx_done`: Called when a message transmission is acknowledged.
  - Flags that specify the client’s configuration, such as whether it operates
    in blocking mode.

- **struct mbox_chan**
  This structure represents an individual mailbox channel. It maintains the
  state required for message queuing and transmission. Key members include:
  - `msg_data`: Array of messages queued for transmission.
  - `msg_count`: Number of messages currently queued.
  - `msg_free`: Index of the next free slot in the message queue.
  - `active_req`: Pointer to the currently active message being transmitted.
  - Synchronization primitives to manage access from multiple contexts.

- **struct mbox_controller**
  This structure represents a mailbox controller that manages multiple
  channels. It includes:
  - A pointer to the device managing the mailbox.
  - Operations for sending and receiving messages, as well as initializing
    and shutting down the mailbox.
  - A list of associated channels and the total number of channels available.

controller framework APIs
=========================

``struct `mbox_controller` Initialization
-----------------------------------------

Just like any other kernel framework, the whole mailbox controller registration
relies on the driver filling a structure and registering against the
framework. In our case, that structure is mbox_controller.

The first thing you need to do in your driver is to allocate this
structure. Any of the usual memory allocators will do, but you'll also
need to initialize a few fields in there:

- ``dev``: should hold the pointer to the ``struct device`` associated
  to your current driver instance.

- ``ops``: Operators that work on each communication channel.

- ``chans``: Array of channels.

- ``num_chans``: Number of channels in the `chans` array.

- ``txdone_irq``: Indicates if the controller can report to the API
  when the last transmitted data was read by the
                          remote (e.g., if it has a TX ACK interrupt).

All the below fields are not mandatory.

- ``txdone_poll``: Indicates if the controller can read but not report
                          the TX done. For example, some register may show
                          the TX status, but no interrupt is raised. This
                          field is ignored if `txdone_irq` is set.

- ``txpoll_period``: If `txdone_poll` is in effect, the API polls for
                          the last TX status after this many milliseconds.

- ``of_xlate``: Controller driver-specific mapping of channel via
                          Device Tree (DT).


Key Functions
-------------

- **int devm_mbox_controller_register(struct mbox_controller *mbox)**
  This function registers a mailbox controller with the kernel. It makes the
  channels associated with the controller available for client requests. The
  function performs sanity checks on the controller structure to ensure all
  necessary fields are populated.

- **struct mbox_chan *mbox_request_channel(struct mbox_client *cl, int index)**
  This function requests a mailbox channel for a specified client, identified
  by an index. It searches for the appropriate mailbox channel, and if found,
  it returns a pointer to the channel. If the request fails (e.g., if the
  index is invalid), it returns an error pointer.

- **void mbox_free_channel(struct mbox_chan *chan)**
  This function releases a mailbox channel that was previously allocated for a
  client. It ensures that the channel can be reused by other clients. If any
  messages are still in the queue, they are aborted, and no callbacks are made.

- **int mbox_send_message(struct mbox_chan *chan, void *mssg)**
  This function is used by clients to send a message through the specified
  mailbox channel. The function can operate in either blocking or non-blocking
  mode, depending on the client’s configuration. It will queue the message for
  transmission and notify the client once the message is acknowledged.

- **void mbox_chan_received_data(struct mbox_chan *chan, void *mssg)**
  This function is called by the controller driver to notify the mailbox
  framework that a message has been received on the specified channel. The
  received message is then passed to the appropriate client's `rx_callback`
  function for processing.
