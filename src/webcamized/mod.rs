use ffmpeg_next as ffmpeg;
use ffmpeg_next::codec::packet::Packet;
use ffmpeg_next::format::Pixel;
use ffmpeg_next::frame::Video;
use ffmpeg_next::software::scaling::{context::Context, flag::Flags};
use ffmpeg_next::util::format::pixel::Pixel as PixelFormat;
use gphoto2::camera::CameraEvent;
use gphoto2::widget::{RadioWidget, TextWidget, ToggleWidget, WidgetBase};
use gphoto2::Context as GPhotoContext;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};
use std::{io, thread};

mod canon;

const TARGET_FPS: i32 = 60;
const FRAME_DURATION: Duration = Duration::from_nanos(1_000_000_000 / TARGET_FPS as u64);

pub(crate) fn init() -> Result<(), Box<dyn std::error::Error>> {
    // Initialize FFmpeg
    ffmpeg::init()?;
    ffmpeg::log::set_level(ffmpeg::log::Level::Verbose);

    // Initialize gphoto2
    let context = GPhotoContext::new()?;
    let camera = context.autodetect_camera().wait()?;

    // Set up Ctrl+C handler
    let running = Arc::new(AtomicBool::new(true));
    let r = running.clone();
    ctrlc::set_handler(move || {
        r.store(false, Ordering::SeqCst);
    })?;

    // Get one preview to determine format
    println!("Getting initial preview to determine format...");
    let first_preview = camera.capture_preview().wait()?;
    let first_data = first_preview.get_data(&context).wait()?;

    // Find a JPEG decoder
    let jpeg_decoder_codec = ffmpeg::decoder::find(ffmpeg::codec::Id::MJPEG)
        .ok_or_else(|| ffmpeg::Error::DecoderNotFound)?;

    let decoder_context = ffmpeg::codec::context::Context::new();
    let decoder = decoder_context.decoder().open_as(jpeg_decoder_codec)?;
    let mut video_decoder = decoder.video()?;

    // Create a packet from the JPEG data
    let packet = Packet::copy(&first_data);

    // Send packet to decoder
    video_decoder.send_packet(&packet)?;

    // Get the decoded frame
    let mut decoded_frame = Video::empty();
    video_decoder.receive_frame(&mut decoded_frame)?;

    // Get dimensions and format
    let width = decoded_frame.width();
    let height = decoded_frame.height();
    let source_format = decoded_frame.format();

    println!("Camera preview size: {}x{}", width, height);
    println!("Source pixel format: {:?}", source_format);

    // Set up V4L2 output
    let device_number = 9;
    let output_path = format!("/dev/video{}", device_number);

    // Create V4L2 output context
    let mut octx = ffmpeg::format::output_as(&output_path, "v4l2")?;

    // Set up encoder for raw video (V4L2 loopback device)
    let codec =
        ffmpeg::encoder::find(ffmpeg::codec::Id::RAWVIDEO).ok_or(ffmpeg::Error::EncoderNotFound)?;

    let mut ost = octx.add_stream(codec)?;
    let mut encoder = ffmpeg::codec::context::Context::new().encoder().video()?;

    // Configure encoder - RGB24 is commonly supported by V4L2
    let output_pixel_format = Pixel::YUV420P;

    encoder.set_format(output_pixel_format);
    encoder.set_width(width);
    encoder.set_height(height);
    encoder.set_time_base((1, TARGET_FPS));
    encoder.set_frame_rate(Some((TARGET_FPS, 1)));
    encoder.set_threading(ffmpeg::threading::Config {
        kind: ffmpeg::threading::Type::Frame,
        count: num_cpus::get(),
    });

    let mut encoder = encoder.open_as(codec)?;
    ost.set_parameters(&encoder);
    octx.write_header()?;

    // Set up scaling context to convert from source format to RGB24
    let mut scaler = Context::get(
        source_format,
        width,
        height,
        output_pixel_format,
        width,
        height,
        Flags::BILINEAR,
    )?;

    // Prepare output frame
    let mut output_frame = Video::new(output_pixel_format, width, height);
    let mut frame_index = 0;
    let mut next_frame_time = Instant::now();

    // Main loop - capture and stream frames
    println!(
        "Starting preview stream to {} (Press Ctrl+C to stop)",
        output_path
    );

    // Create decoder once instead of for each frame
    let jpeg_decoder_codec = ffmpeg::decoder::find(ffmpeg::codec::Id::MJPEG)
        .ok_or_else(|| ffmpeg::Error::DecoderNotFound)?;

    while running.load(Ordering::SeqCst) {
        // Capture preview from camera
        let preview = match camera.capture_preview().wait() {
            Ok(p) => p,
            Err(e) => {
                eprintln!("Error capturing preview: {}", e);
                continue;
            }
        };

        let data = match preview.get_data(&context).wait() {
            Ok(d) => d,
            Err(e) => {
                eprintln!("Error getting preview data: {}", e);
                continue;
            }
        };

        // Create a new decoder context for each frame
        let decoder_context = ffmpeg::codec::context::Context::new();
        let decoder = match decoder_context.decoder().open_as(jpeg_decoder_codec) {
            Ok(d) => d,
            Err(e) => {
                eprintln!("Error creating decoder: {}", e);
                continue;
            }
        };

        let mut video_decoder = match decoder.video() {
            Ok(vd) => vd,
            Err(e) => {
                eprintln!("Error getting video decoder: {}", e);
                continue;
            }
        };

        // Create a packet from the JPEG data
        let packet = Packet::copy(&data);

        // Send packet to decoder
        if let Err(e) = video_decoder.send_packet(&packet) {
            eprintln!("Error sending packet to decoder: {}", e);
            continue;
        }

        // Get the decoded frame
        let mut decoded_frame = Video::empty();
        if let Err(e) = video_decoder.receive_frame(&mut decoded_frame) {
            eprintln!("Error receiving frame: {}", e);
            continue;
        }

        // Scale/convert the frame to RGB24 for V4L2
        if let Err(e) = scaler.run(&decoded_frame, &mut output_frame) {
            eprintln!("Error scaling frame: {}", e);
            continue;
        }

        // Set frame timing
        output_frame.set_pts(Some(frame_index));
        frame_index += 1;

        // Encode and send to V4L2
        if let Err(e) = encoder.send_frame(&output_frame) {
            eprintln!("Error sending frame to encoder: {}", e);
            continue;
        }

        let mut encode_packet = ffmpeg::codec::packet::Packet::empty();
        while encoder.receive_packet(&mut encode_packet).is_ok() {
            encode_packet.set_stream(0);
            if let Err(e) = encode_packet.write_interleaved(&mut octx) {
                eprintln!("Error writing packet: {}", e);
            }
        }

        // Frame rate control
        next_frame_time += FRAME_DURATION;
        let now = Instant::now();
        if next_frame_time > now {
            thread::sleep(next_frame_time - now);
        } else {
            // We're behind schedule, catch up
            next_frame_time = now;
        }
    }

    // Clean up
    println!("\nShutting down...");
    encoder.send_eof()?;
    let mut packet = ffmpeg::codec::packet::Packet::empty();
    while encoder.receive_packet(&mut packet).is_ok() {
        packet.set_stream(0);
        packet.write_interleaved(&mut octx)?;
    }

    octx.write_trailer()?;
    println!("Stream stopped");

    Ok(())
}
