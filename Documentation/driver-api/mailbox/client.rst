Mailbox Client Documentation
============================

Overview
--------
The mailbox client driver is responsible for sending and receiving messages
to and from a remote processor. It uses mailbox APIs provided by the
mailbox framework.

Mailbox Structure
-----------------
The mailbox structure is defined as follows:

.. code-block:: c

   struct mbox_client {
       //device associated with the mailbox
       struct device *dev;
       // callback for transmission completion
       void (*tx_done)(struct mbox_client *client);
       // callback to prepare for sending a message
       void (*tx_prepare)(struct mbox_client *client);
       // callback for received messages
       void (*rx_callback)(struct mbox_client *client, void *data);
       // flag to indicate if transmission should block
       bool tx_block;
       // indicates if the client knows when transmission is done
       bool knows_txdone;
   };

Key Functions
-------------
1. Requesting a Mailbox Channel
   - **Function**: `mbox_request_channel(struct mbox_client *client,
     unsigned int channel)`
   - **Description**: Requests a mailbox channel for sending messages.
   - **Parameters**:
     - `client`: Pointer to the mailbox client structure.
     - `channel`: The specific mailbox channel to request.
   - **Returns**: A pointer to the mailbox channel on success, or an error
     code on failure.

2. Sending a Message
   - **Function**: `mbox_send_message(struct mbox_chan *chan, void *msg)`
   - **Description**: Sends a message through the mailbox channel.
   - **Parameters**:
     - `chan`: The mailbox channel used for communication.
     - `msg`: Pointer to the message to be sent (usually NULL for dummy
       messages).
   - **Returns**: 0 on success, or a negative error code on failure.

3. Transmitting Completion
   - **Function**: `mbox_client_txdone(struct mbox_chan *chan, unsigned int
     msg_id)`
   - **Description**: Notifies the mailbox framework that message
     transmission is complete.
   - **Parameters**:
     - `chan`: The mailbox channel associated with the message.
     - `msg_id`: The identifier of the message that was transmitted.

Usage Example
-------------
In a typical mailbox client driver, the following steps are typically
performed:

1. Initialize the Mailbox Client:

   .. code-block:: c

      struct mbox_client my_mbox_client = {
          .dev = &my_device,
          .tx_done = my_tx_done_callback,
          .rx_callback = my_rx_callback,
          .tx_block = false,
          .knows_txdone = true,
      };

2. Request a Mailbox Channel:

   .. code-block:: c

      mbox_chan = mbox_request_channel(&my_mbox_client, 0);
      if (IS_ERR(mbox_chan)) {
          // Handle error
      }

3. Send a Message:

   .. code-block:: c

      int ret = mbox_send_message(mbox_chan, NULL); // Sending a dummy message
      if (ret < 0) {
          // Handle error
      }

4. Complete Transmission:

   .. code-block:: c

      mbox_client_txdone(mbox_chan, 0);

Interrupt Handling
------------------
The mailbox interface can trigger interrupts upon message receipt. Handlers
should be implemented in the `rx_callback` function defined in the mailbox
client structure to process incoming messages.

Example Mailbox Client Driver
-----------------------------
.. code-block:: c

   struct demo_client {
       struct mbox_client cl;
       struct mbox_chan *mbox;
       struct completion c;
       bool async;
       /* ... */
   };

   /*
   * This is the handler for data received from remote. The behaviour is purely
   * dependent upon the protocol. This is just an example.
   */
   static void message_from_remote(struct mbox_client *cl, void *mssg)
   {
       struct demo_client *dc = container_of(cl, struct demo_client, cl);
       if (dc->async) {
           if (is_an_ack(mssg)) {
               /* An ACK to our last sample sent */
               return; /* Or do something else here */
           } else { /* A new message from remote */
               queue_req(mssg);
           }
       } else {
           /* Remote f/w sends only ACK packets on this channel */
           return;
       }
   }

   static void sample_sent(struct mbox_client *cl, void *mssg, int r)
   {
       struct demo_client *dc = container_of(cl, struct demo_client, cl);
       complete(&dc->c);
   }

   static void client_demo(struct platform_device *pdev)
   {
       struct demo_client *dc_sync, *dc_async;
       /* The controller already knows async_pkt and sync_pkt */
       struct async_pkt ap;
       struct sync_pkt sp;

       dc_sync = kzalloc(sizeof(*dc_sync), GFP_KERNEL);
       dc_async = kzalloc(sizeof(*dc_async), GFP_KERNEL);

       /* Populate non-blocking mode client */
       dc_async->cl.dev = &pdev->dev;
       dc_async->cl.rx_callback = message_from_remote;
       dc_async->cl.tx_done = sample_sent;
       dc_async->cl.tx_block = false;
       dc_async->cl.tx_tout = 0; /* doesn't matter here */
       dc_async->cl.knows_txdone = false; /* depending upon protocol */
       dc_async->async = true;
       init_completion(&dc_async->c);

       /* Populate blocking mode client */
       dc_sync->cl.dev = &pdev->dev;
       dc_sync->cl.rx_callback = message_from_remote;
       dc_sync->cl.tx_done = NULL; /* operate in blocking mode */
       dc_sync->cl.tx_block = true;
       dc_sync->cl.tx_tout = 500; /* by half a second */
       dc_sync->cl.knows_txdone = false; /* depending upon protocol */
       dc_sync->async = false;

       /* ASync mailbox is listed second in 'mboxes' property */
       dc_async->mbox = mbox_request_channel(&dc_async->cl, 1);
       /* Populate data packet */
       /* ap.xxx = 123; etc */
       /* Send async message to remote */
       mbox_send_message(dc_async->mbox, &ap);

       /* Sync mailbox is listed first in 'mboxes' property */
       dc_sync->mbox = mbox_request_channel(&dc_sync->cl, 0);
       /* Populate data packet */
       /* sp.abc = 123; etc */
       /* Send message to remote in blocking mode */
       mbox_send_message(dc_sync->mbox, &sp);
       /* At this point 'sp' has been sent */

       /* Now wait for async chan to be done */
       wait_for_completion(&dc_async->c);
   }
