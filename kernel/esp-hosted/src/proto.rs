//! The few RPC messages an access point needs, as protobuf by hand: `esp_hosted_rpc.proto` of
//! esp-hosted-mcu.

use alloc::vec::Vec;

pub const SET_MODE: u32 = 260;
pub const GET_MAC_ADDRESS: u32 = 257;
pub const WIFI_INIT: u32 = 278;
pub const WIFI_START: u32 = 280;
pub const WIFI_SET_CONFIG: u32 = 284;
pub const EVENT_ESP_INIT: u64 = 769;

pub enum Value<'a> {
    Varint(u64),
    Bytes(&'a [u8]),
}

fn varint(out: &mut Vec<u8>, mut value: u64) {
    while value >= 0x80 {
        out.push((value & 0x7f) as u8 | 0x80);
        value >>= 7;
    }
    out.push((value & 0x7f) as u8);
}

pub fn number(out: &mut Vec<u8>, field: u32, value: u64) {
    varint(out, u64::from(field) << 3);
    varint(out, value);
}

pub fn bytes(out: &mut Vec<u8>, field: u32, value: &[u8]) {
    varint(out, (u64::from(field) << 3) | 2);
    varint(out, value.len() as u64);
    out.extend_from_slice(value);
}

/// An `Rpc` request: `message` is the payload, in the field numbered like the request's id.
pub fn request(id: u32, message: &[u8]) -> Vec<u8> {
    let mut out = Vec::new();
    number(&mut out, 1, 1);
    number(&mut out, 2, id.into());
    bytes(&mut out, id, message);
    out
}

/// The fields of a message, ending at the first malformed one. The protocol has no fixed-width
/// fields.
pub fn fields(mut message: &[u8]) -> impl Iterator<Item = (u32, Value<'_>)> {
    fn read(message: &mut &[u8]) -> Option<u64> {
        let mut value = 0;
        for shift in (0..64).step_by(7) {
            let (&byte, rest) = message.split_first()?;
            *message = rest;
            value |= u64::from(byte & 0x7f) << shift;
            if byte < 0x80 {
                return Some(value);
            }
        }
        None
    }
    core::iter::from_fn(move || {
        let key = read(&mut message)?;
        let value = match key & 7 {
            0 => Value::Varint(read(&mut message)?),
            2 => {
                let size = usize::try_from(read(&mut message)?).ok()?;
                let (value, rest) = message.split_at_checked(size)?;
                message = rest;
                Value::Bytes(value)
            }
            _ => return None,
        };
        Some((u32::try_from(key >> 3).ok()?, value))
    })
}

pub fn find(message: &[u8], field: u32) -> Option<Value<'_>> {
    fields(message).find_map(|(number, value)| (number == field).then_some(value))
}
