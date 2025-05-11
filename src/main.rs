use std::path::PathBuf;

use clap::{Parser, Subcommand};

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
    command: Option<Commands>,
}

#[derive(Subcommand)]
enum Commands {
    /// Starts the webcam
    Start {
        /// Sets the /dev/video_ device number used as the sink
        #[arg(short, long)]
        device_number: u16,
    },

    /// Stops the webcam
    Stop {
        /// lists test values
        #[arg(short, long)]
        list: bool,
    },

    /// Opens the Webcamize control panel
    Panel {
        /// lists test values
        #[arg(short, long)]
        list: bool,
    },

    /// Reports the status of webcamize
    Status {
        /// lists test values
        #[arg(short, long)]
        list: bool,
    },

    #[clap(hide = true)]
    /// Starts webcamize as a daemon
    Daemon {
        /// lists test values
        #[arg(short, long)]
        list: bool,
    },
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

    // You can check for the existence of subcommands, and if found use their
    // matches just as you would the top level cmd
    match &cli.command {
        Some(Commands::Daemon { list }) => webcamized::init().unwrap(),
        _ => {}
    }

    // Continued program logic goes here...
}
