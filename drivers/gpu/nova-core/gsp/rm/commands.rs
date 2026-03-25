// SPDX-License-Identifier: GPL-2.0

use core::array;

use kernel::prelude::*;

use crate::{
    driver::Bar0,
    gsp::{
        cmdq::{
            Cmdq,
            CommandToGsp,
            MessageFromGsp, //
        },
        commands::{
            Client,
            Handle, //
        },
        fw::{
            rm::*,
            MsgFunction,
            NvStatus, //
        },
    },
    sbuffer::SBufferIter, //
};

/// Wraps a [`RmControl`] command to provide the infrastructure for sending it on the command queue.
struct RawRmControl<'a, T>(&'a RmControl<T>)
where
    T: RmControlCommand;

impl<'a, T: RmControlCommand> CommandToGsp for RawRmControl<'a, T> {
    const FUNCTION: MsgFunction = MsgFunction::GspRmControl;
    type Command = GspRmControl;
    type Reply = RmControlReply;
    type InitError = Error;

    fn init(&self) -> impl Init<Self::Command, Self::InitError> {
        let params_size: u32 = T::LEN.try_into()?;
        Ok(GspRmControl::new(
            self.0.client,
            self.0.object,
            T::FUNCTION,
            params_size,
        ))
    }

    fn variable_payload_len(&self) -> usize {
        T::LEN
    }

    fn init_variable_payload(
        &self,
        dst: &mut SBufferIter<array::IntoIter<&mut [u8], 2>>,
    ) -> Result {
        self.0.cmd.write_payload(dst)
    }
}

/// Command for sending an RM control message to the GSP.
///
/// RM control messages are used to query or control RM objects (see [`Handle`] for more info on RM
/// objects). It takes a client handle and an RM object handle identifying the target of the
/// message, within the given client.
pub(crate) struct RmControl<T>
where
    T: RmControlCommand,
{
    /// The client handle under which `object` is allocated.
    client: Handle<Client>,
    /// The RM object handle to query or control.
    object: Handle<T::Target>,
    /// The specific RM control command to send.
    cmd: T,
}

#[expect(unused)]
impl<T: RmControlCommand> RmControl<T> {
    /// Creates a new RM control command.
    pub(crate) fn new(client: Handle<Client>, object: Handle<T::Target>, cmd: T) -> Self {
        Self {
            client,
            object,
            cmd,
        }
    }

    /// Sends an RM control command, checks the reply status, and returns the reply.
    pub(crate) fn send(self, cmdq: &Cmdq, bar: &Bar0) -> Result<T::Reply> {
        let reply = cmdq.send_command(bar, RawRmControl(&self))?;

        Result::from(reply.status)?;

        self.cmd.parse_reply(&reply.params)
    }
}

/// Response from an RM control message.
struct RmControlReply {
    status: NvStatus,
    params: KVVec<u8>,
}

impl MessageFromGsp for RmControlReply {
    const FUNCTION: MsgFunction = MsgFunction::GspRmControl;
    type Message = GspRmControl;
    type InitError = Error;

    fn read(
        msg: &Self::Message,
        sbuffer: &mut SBufferIter<array::IntoIter<&[u8], 2>>,
    ) -> Result<Self, Self::InitError> {
        Ok(RmControlReply {
            status: msg.status(),
            params: sbuffer.read_to_vec(GFP_KERNEL)?,
        })
    }
}

/// Trait for RM control commands that can be sent using [`RmControl`].
pub(crate) trait RmControlCommand {
    /// The specific RM control message kind to send.
    const FUNCTION: RmControlMsgFunction;

    /// Length of the command-specific payload to send with the RM control message.
    const LEN: usize;

    /// The type of the reply after parsing the raw bytes from the RM control reply message.
    type Reply;

    /// The type of the RM object that this command targets.
    type Target;

    /// Writes the payload of the command into `dst`.
    fn write_payload(&self, dst: &mut SBufferIter<array::IntoIter<&mut [u8], 2>>) -> Result;

    /// Parses the reply bytes from an RM control reply message into the command-specific
    /// reply type.
    fn parse_reply(&self, params: &[u8]) -> Result<Self::Reply>;
}
