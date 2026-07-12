use std::ffi::{c_char, c_void, CStr, CString};
use std::fs;
use std::mem::{offset_of, size_of};
use std::path::PathBuf;
use std::ptr;

#[repr(C)]
struct WeSourceV1 {
    size: u32,
    version: u32,
    uri: *const c_char,
    assets_uri: *const c_char,
    fps: i32,
    speed: f32,
    volume: f32,
    muted: u8,
    options_json: *const c_char,
}

#[repr(C)]
struct LegacyWeSourceV1 {
    size: u32,
    version: u32,
    uri: *const c_char,
    assets_uri: *const c_char,
    fps: i32,
    speed: f32,
    volume: f32,
    muted: u8,
}

#[repr(C)]
struct WeRenderConfigV1 {
    size: u32,
    version: u32,
    width: u32,
    height: u32,
    enable_valid_layer: u8,
    prefer_dmabuf: u8,
    allow_shm_fallback: u8,
    msaa_samples: u32,
}

#[repr(C)]
struct LegacyWeRenderConfigV1 {
    size: u32,
    version: u32,
    width: u32,
    height: u32,
    enable_valid_layer: u8,
    prefer_dmabuf: u8,
    allow_shm_fallback: u8,
}

unsafe extern "C" {
    fn we_session_create() -> *mut c_void;
    fn we_session_destroy(session: *mut c_void);
    fn we_session_get_frame_ready_fd(session: *mut c_void) -> i32;
    fn we_session_set_source(session: *mut c_void, source: *const WeSourceV1) -> i32;
    fn we_session_set_user_properties_json(session: *mut c_void, json: *const c_char) -> i32;
    fn we_session_get_diagnostics_json(
        session: *mut c_void,
        buffer: *mut c_char,
        inout_size: *mut u32,
    ) -> i32;
}

fn fixture_dir() -> PathBuf {
    let path = std::env::temp_dir().join(format!(
        "we-renderer-rust-abi-test-{}",
        std::process::id()
    ));
    let _ = fs::remove_dir_all(&path);
    fs::create_dir_all(&path).expect("create Rust ABI fixture directory");
    fs::write(
        path.join("project.json"),
        r#"{"type":"scene","file":"scene.json","title":"Rust ABI Test"}"#,
    )
    .expect("write project.json");
    path
}

unsafe fn diagnostics(session: *mut c_void) -> String {
    let mut required = 0_u32;
    assert_eq!(
        unsafe { we_session_get_diagnostics_json(session, ptr::null_mut(), &mut required) },
        0
    );
    assert!(required > 1);

    let mut bytes = vec![0_u8; required as usize];
    let mut actual = required;
    assert_eq!(
        unsafe {
            we_session_get_diagnostics_json(
                session,
                bytes.as_mut_ptr().cast::<c_char>(),
                &mut actual,
            )
        },
        0
    );
    assert_eq!(actual, required);
    unsafe { CStr::from_ptr(bytes.as_ptr().cast::<c_char>()) }
        .to_str()
        .expect("diagnostics must be UTF-8 JSON")
        .to_owned()
}

fn main() {
    assert!(offset_of!(WeSourceV1, options_json) >= size_of::<LegacyWeSourceV1>());
    assert!(
        offset_of!(WeRenderConfigV1, msaa_samples) >= size_of::<LegacyWeRenderConfigV1>()
    );

    let fixture = fixture_dir();
    let uri = CString::new(fixture.to_string_lossy().as_bytes()).expect("fixture path CString");

    unsafe {
        let legacy_session = we_session_create();
        assert!(!legacy_session.is_null());
        let legacy_source = LegacyWeSourceV1 {
            size: size_of::<LegacyWeSourceV1>() as u32,
            version: 1,
            uri: uri.as_ptr(),
            assets_uri: ptr::null(),
            fps: 30,
            speed: 1.0,
            volume: 1.0,
            muted: 0,
        };
        let legacy_result = we_session_set_source(
            legacy_session,
            (&legacy_source as *const LegacyWeSourceV1).cast::<WeSourceV1>(),
        );
        assert_ne!(legacy_result, -1, "legacy source prefix was rejected as malformed");
        we_session_destroy(legacy_session);

        let session = we_session_create();
        assert!(!session.is_null());
        assert!(we_session_get_frame_ready_fd(session) >= 0);
        let options = CString::new(r#"{"version":2}"#).expect("options CString");
        let source = WeSourceV1 {
            size: size_of::<WeSourceV1>() as u32,
            version: 1,
            uri: uri.as_ptr(),
            assets_uri: ptr::null(),
            fps: 30,
            speed: 1.0,
            volume: 1.0,
            muted: 0,
            options_json: options.as_ptr(),
        };
        assert!(we_session_set_source(session, &source) > 0);

        let user_properties = CString::new(r#"{"enabled":true}"#).expect("properties CString");
        assert_eq!(
            we_session_set_user_properties_json(session, user_properties.as_ptr()),
            -1,
            "properties before a successful source load must be rejected"
        );

        let payload = diagnostics(session);
        assert!(payload.contains("\"version\":1"));
        assert!(payload.contains("abi.source.options"));
        assert!(payload.contains("unsupported source options version"));
        we_session_destroy(session);
    }

    fs::remove_dir_all(fixture).expect("remove Rust ABI fixture directory");
}
