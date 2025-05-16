use clap::{ArgAction, Parser, Subcommand};
use interprocess::local_socket::{traits::Stream, GenericNamespaced, ToNsName};
use std::io::{BufRead, BufReader, Read, Write};

mod webcamized;

#[derive(Parser)]
#[command(
    version,
    about,
    long_about = None
)]
struct Cli {
    /// Use a custom config file
    //#[arg(short, long, value_name = "FILE")]
    //config: Option<PathBuf>,

    /// Sets the log level
    //#[arg(short, long, action = clap::ArgAction::Count)]
    //log_level: u8,

    #[command(subcommand)]
    command: Commands,
}

#[derive(Subcommand)]
enum Commands {
    /// Starts the webcam (default: autodetects the camera)
    Start {
        /// Start all available cameras
        #[clap(short, long, action=ArgAction::SetFalse)]
        all: Option<bool>,
        /// Sets the /dev/video_ device number
        #[arg(short, long)]
        device_number: Option<u16>,
        /// Start a camera by model name
        #[arg(short = 'm', long)]
        camera_model: Option<String>,
        /// The port the camera to start is plugged into
        #[arg(short = 'p', long)]
        camera_port: Option<String>,
    },

    /// Stops the webcam (default: stops all webcams)
    Stop {
        /// Stop all available cameras (default: true)
        #[clap(short, long, action=ArgAction::SetTrue)]
        all: Option<bool>,
        /// Stops the camera on the /dev/video_ device number
        #[arg(short, long)]
        device_number: Option<u16>,
        /// The camera model to stop
        #[arg(short = 'm', long)]
        camera_model: Option<String>,
        /// The port to stop
        #[arg(short = 'p', long)]
        camera_port: Option<String>,
    },

    /// Opens the Webcamize control panel in your web browser
    Panel {
        /// lists test values
        #[arg(short, long)]
        list: bool,
    },

    /// Reports the status of Webcamize
    Status {},

    #[clap(hide = true)]
    /// Starts webcamize as a daemon
    Daemon {},
}

fn main() {
    let cli = Cli::parse();

    //if let Some(config_path) = cli.config.as_deref() {
    //    println!("Value for config: {}", config_path.display());
    //}

    // You can see how many times a particular flag or argument occurred
    // Note, only flags can have multiple occurrences
    // match cli.debug {
    //    0 => println!("Debug mode is off"),
    //    1 => println!("Debug mode is kind of on"),
    //    2 => println!("Debug mode is on"),
    //    _ => println!("Don't be crazy"),
    //}

    match &cli.command {
        Commands::Daemon {} => webcamized::init().unwrap(),
        Commands::Status {} => status(),
        Commands::Start {
            all,
            device_number,
            camera_model,
            camera_port,
        } => start(),

        _ => {}
    }

    // Continued program logic goes here...
}

fn connect_to_daemon() -> Result<interprocess::local_socket::Stream, std::io::Error> {
    let name = "webcamize.sock".to_ns_name::<GenericNamespaced>().unwrap();
    interprocess::local_socket::Stream::connect(name)
}

fn start() {
    let conn_res = connect_to_daemon();
    let mut buffer = Vec::with_capacity(512);
    match conn_res {
        Ok(conn) => {
            let mut conn = BufReader::new(conn);
            conn.get_mut().write_all(b"start\0").unwrap();
            conn.read_until(b'\0', &mut buffer).unwrap();
            buffer.pop(); // remove null terminator
            println!("{}", String::from_utf8(buffer).unwrap());
        }
        Err(_) => println!("  Daemon is not running!"),
    }
}

fn status() {
    println!("Webcamize {}", env!("CARGO_PKG_VERSION"));
    println!("");

    // libs
    {
        println!("System libraries:");
        println!("  libgphoto2: {}", gphoto2::library_version().unwrap());

        {
            fn format_ffmpeg_ver(v: u32) -> String {
                format!("{}.{}.{}", v >> 16, (v >> 8) & 0xFF, v & 0xFF)
            }
            println!("  ffmpeg:");
            println!(
                "    libavutil: {}",
                format_ffmpeg_ver(ffmpeg_next::util::version())
            );
            println!(
                "    libavformat: {}",
                format_ffmpeg_ver(ffmpeg_next::codec::version())
            );
            println!(
                "    libavformat: {}",
                format_ffmpeg_ver(ffmpeg_next::format::version())
            );
            println!(
                "    libavdevice: {}",
                format_ffmpeg_ver(ffmpeg_next::device::version())
            );
            println!(
                "    libavfilter: {}",
                format_ffmpeg_ver(ffmpeg_next::filter::version())
            );
            println!(
                "    libswscale: {}",
                format_ffmpeg_ver(ffmpeg_next::software::scaling::version())
            );
            println!(
                "    libswresample: {}",
                format_ffmpeg_ver(ffmpeg_next::software::resampling::version())
            );
        }
    }

    println!("");

    // daemon
    {
        println!("Daemon status:");
        let conn_res = connect_to_daemon();
        let mut buffer = Vec::with_capacity(512);
        match conn_res {
            Ok(conn) => {
                let mut conn = BufReader::new(conn);
                conn.get_mut().write_all(b"status\0").unwrap();
                conn.read_until(b'\0', &mut buffer).unwrap();
                buffer.pop(); // remove null terminator
                println!("{}", String::from_utf8(buffer).unwrap());
            }
            Err(_) => println!("  Daemon is not running!"),
        }
    }

    println!("");

    // gphotos
    {
        let gpctx = gphoto2::Context::new().unwrap();
        println!("Detected cameras:");
        let cams = gpctx.list_cameras().wait().unwrap();
        if cams.len() == 0 {
            println!("  No cameras detected!")
        } else {
            let mut i = 1;
            for cd in cams {
                let cam = gpctx.get_camera(&cd).wait().unwrap();
                println!(
                    "  {}: {} ({}), {}",
                    i,
                    cd.model,
                    cd.port,
                    cam.abilities().id()
                );

                if cam.abilities().camera_operations().capture_video() {
                    println!("     Supported (Video)");
                } else if cam.abilities().camera_operations().capture_preview() {
                    println!("     Supported (Preview)");
                } else {
                    println!("     Support unknown");
                }
                println!("");

                i = i + 1;
            }
        }
    }

    println!("");
}
