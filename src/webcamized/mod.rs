use ffmpeg_next as ffmpeg;
use ffmpeg_next::frame::Video;
use gphoto2::Context;
use std::sync::atomic::{AtomicBool, AtomicPtr, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};

const TARGET_FPS: i32 = 60;
const FRAME_DURATION: Duration = Duration::from_nanos(1_000_000_000 / TARGET_FPS as u64);

pub(crate) fn init() -> Result<(), Box<dyn std::error::Error>> {
    // Initialize FFmpeg
    ffmpeg::init()?;
    //ffmpeg::log::set_level(ffmpeg::log::Level::Warning);

    // Initialize gphoto2
    let context = Context::new()?;
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

    // Parse the image to get dimensions
    let img = image::load_from_memory(&first_data)?;
    let width = img.width();
    let height = img.height();
    println!("Camera preview size: {}x{}", width, height);

    // Set up V4L2 output
    let device_number = 9;
    let output_path = format!("/dev/video{}", device_number);

    // Create V4L2 output context
    let mut octx = ffmpeg::format::output_as(&output_path, "v4l2")?;

    // Set up encoder for raw video
    let codec =
        ffmpeg::encoder::find(ffmpeg::codec::Id::RAWVIDEO).ok_or(ffmpeg::Error::EncoderNotFound)?;

    let mut ost = octx.add_stream(codec)?;
    let mut encoder = ffmpeg::codec::context::Context::new().encoder().video()?;

    // Configure encoder
    encoder.set_format(ffmpeg::format::Pixel::RGB24);
    encoder.set_width(width);
    encoder.set_height(height);
    encoder.set_time_base((1, TARGET_FPS)); // 60 fps
    encoder.set_frame_rate(Some((TARGET_FPS, 1)));
    encoder.set_threading(ffmpeg::threading::Config {
        kind: ffmpeg::threading::Type::Frame,
        count: num_cpus::get(),
    });

    let mut encoder = encoder.open_as(codec)?;
    ost.set_parameters(&encoder);
    octx.write_header()?;

    // Set up frame converter (RGB to YUV420P)
    //let mut scaler = ffmpeg::software::scaling::context::Context::get(
    //    ffmpeg::format::Pixel::RGB24,
    //    width,
    //    height,
    //    ffmpeg::format::Pixel::RGB24,
    //    width,
    //    height,
    //    ffmpeg::software::scaling::flag::Flags::BILINEAR,
    //)?;

    let mut frame_index = 0;

    // Main loop - capture and stream frames
    println!(
        "Starting preview stream to {} (Press Ctrl+C to stop)",
        output_path
    );

    // frame buffer
    #[allow(unused_assignments)]
    let mut input_frame =
        ffmpeg::util::frame::video::Video::new(ffmpeg::format::Pixel::RGB24, width, height);
    #[warn(unused_assignments)]
    let mut next_frame_time = Instant::now();

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
        // Decode the preview image (usually JPEG)
        let img = match image::load_from_memory(&data) {
            Ok(i) => i,
            Err(e) => {
                eprintln!("Error decoding image: {}", e);
                continue;
            }
        };
        let rgb_img = img.to_rgb8();
        // Create FFmpeg frame from image data
        input_frame =
            ffmpeg::util::frame::video::Video::new(ffmpeg::format::Pixel::RGB24, width, height);

        // Copy RGB data to frame
        {
            let frame_data = input_frame.data_mut(0);
            let rgb_data = rgb_img.as_raw();

            unsafe {
                std::ptr::copy_nonoverlapping(
                    rgb_data.as_ptr(),
                    frame_data.as_mut_ptr(),
                    rgb_data.len(),
                );
            }
        }

        // Convert to YUV420P
        // let mut output_frame = ffmpeg::util::frame::video::Video::empty();
        //scaler.run(&input_frame, &mut output_frame).unwrap();

        // Set frame timing
        input_frame.set_pts(Some(frame_index));
        frame_index += 1;

        // Encode and send to V4L2
        encoder.send_frame(&input_frame).unwrap();

        let mut packet = ffmpeg::codec::packet::Packet::empty();
        while encoder.receive_packet(&mut packet).is_ok() {
            packet.set_stream(0);
            packet.write_interleaved(&mut octx).unwrap();
        }

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
