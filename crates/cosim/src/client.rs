//! One connection to `--cosim-tcp`: one device, as the emulator counts them.

use std::io::{self, BufReader};
use std::net::TcpStream;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use crate::{decode, read_frame, write_frame, Message};

/// The socket, written from any thread (answers, pin drives) and read from
/// one.
pub struct Client {
    writer: Arc<Mutex<TcpStream>>,
    reader: BufReader<TcpStream>,
}

impl Client {
    /// Connect, waiting up to `wait` for an emulator still starting.
    pub fn connect(addr: &str, wait: Duration) -> io::Result<Client> {
        let start = std::time::Instant::now();
        let stream = loop {
            match TcpStream::connect(addr) {
                Ok(s) => break s,
                Err(e) if start.elapsed() >= wait => return Err(e),
                Err(_) => std::thread::sleep(Duration::from_millis(50)),
            }
        };
        stream.set_nodelay(true)?;
        Ok(Client { writer: Arc::new(Mutex::new(stream.try_clone()?)), reader: BufReader::new(stream) })
    }

    /// A handle to send with while another thread reads.
    pub fn sender(&self) -> Sender {
        Sender(self.writer.clone())
    }

    pub fn send(&self, msg: &[u8]) -> io::Result<()> {
        self.sender().send(msg)
    }

    /// The next message from the emulator; None once it has gone. Messages
    /// that do not decode are skipped.
    pub fn next_message(&mut self) -> io::Result<Option<Message>> {
        loop {
            match read_frame(&mut self.reader)? {
                None => return Ok(None),
                Some((_topic, msg)) => {
                    if let Some(m) = decode(&msg) {
                        return Ok(Some(m));
                    }
                }
            }
        }
    }
}

#[derive(Clone)]
pub struct Sender(Arc<Mutex<TcpStream>>);

impl Sender {
    pub fn send(&self, msg: &[u8]) -> io::Result<()> {
        let mut s = self.0.lock().unwrap_or_else(|e| e.into_inner());
        write_frame(&mut *s, msg)
    }
}
