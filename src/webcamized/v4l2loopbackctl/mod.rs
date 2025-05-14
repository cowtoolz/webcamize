use std::{io, process::Command};

pub(crate) fn add_camera() -> Result<String, Box<dyn std::error::Error>> {
    let cmd = Command::new("v4l2loopback-ctl").arg("add").output()?;

    let out: String;
    unsafe {
        out = String::from_utf8_unchecked(cmd.stdout);
    }
    Ok(out)
}
