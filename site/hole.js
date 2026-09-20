// The same black hole the program draws, moved to the browser.
//
// This is a port of src/ui/AuroraWidget.cpp rather than a lookalike. The
// fragment shader below is that file's shader with its GLSL 3.30 header
// swapped for the GLSL ES 3.00 one WebGL2 wants; the ray marching, the
// Doppler boost on the approaching side of the disk, the photon ring and the
// starfield are unchanged. A site that merely resembled the program would
// start drifting from it the first time either one was touched.
//
// One thing is deliberately different. The program hard-codes a field of view
// of 1.22, which suits a window with a whole client drawn over it: the hole is
// scenery there, and it is meant to stay out of the way. A landing page has
// the opposite job, so the same number is a uniform here and set tighter,
// which frames the hole the way the banner does. Nothing about the physics
// changes - it is where the camera stands, not what it is looking at.
//
// The colours are the ones the shipped theme actually produces. Singularity's
// default seed is #121212, which has no hue, and a hue-less seed would scale
// the disk to black - so Theme::storeHole takes a silver branch instead. These
// three vectors are that branch, copied from src/ui/Theme.cpp.
const ACCENT = [0.82, 0.90, 1.00];
const DISK   = [0.62, 0.78, 1.00];
const GRADE  = [1.02, 1.06, 1.16];

const VERT = `#version 300 es
void main() {
    vec2 verts[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(verts[gl_VertexID], 0.0, 1.0);
}`;

const FRAG = (steps) => `#version 300 es
precision highp float;

uniform vec2  uResolution;
uniform float uTime;
uniform vec2  uPointer;
uniform float uZoom;
uniform vec3  uAccent;
uniform vec3  uDisk;
uniform vec3  uGrade;
out vec4 fragColor;

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec3 starfield(vec3 rd) {
    vec3 col = vec3(0.0015, 0.0022, 0.0060);

    float band = exp(-11.0 * rd.y * rd.y);
    col += mix(vec3(0.028, 0.040, 0.070), uAccent * 0.08, 0.55) * band;
    col += uAccent * 0.04 * band * hash12(rd.xz * 8.0);

    float az = atan(rd.x, rd.z);
    float el = asin(clamp(rd.y, -1.0, 1.0));

    for (int k = 0; k < 5; ++k) {
        float sc = exp2(float(k) * 1.18 + 4.05);
        vec2 uv = vec2(az, el) * sc;
        vec2 id = floor(uv);
        vec2 f  = fract(uv) - 0.5;
        float h = hash12(id + float(k) * 17.13);
        if (h > 0.905) {
            float d = length(f);
            float br = smoothstep(0.080 + h * 0.035, 0.0, d);
            float tw = 0.78 + 0.22 * sin(uTime * (0.7 + h * 3.2) + h * 40.0);
            vec3 tc = mix(mix(vec3(0.75, 0.82, 0.95), uAccent, 0.35), vec3(0.95, 0.97, 1.0), fract(h * 9.1));
            if (fract(h * 13.7) > 0.82)
                tc = mix(vec3(0.55, 0.70, 1.0), uAccent, 0.45);
            col += br * tc * tw * (0.28 + 2.4 * pow(h, 9.0));

            if (h > 0.988) {
                col += mix(vec3(0.75, 0.88, 1.0), uAccent, 0.4) * 0.35 * exp(-abs(f.x) * 55.0) * exp(-abs(f.y) * 10.0);
                col += mix(vec3(0.75, 0.88, 1.0), uAccent, 0.4) * 0.18 * exp(-abs(f.y) * 55.0) * exp(-abs(f.x) * 10.0);
            }
        }
    }
    return col;
}

void main() {
    vec2 uv = (gl_FragCoord.xy - 0.5 * uResolution) / uResolution.y;

    float t = uTime * 0.055;
    float cs = cos(t);
    float sn = sin(t);

    vec3 ro = vec3(0.0, 0.62 + uPointer.y * 0.08, 7.15);
    ro.xz = mat2(cs, -sn, sn, cs) * ro.xz;

    vec3 ta = vec3(uPointer.x * 0.08, 0.02, 0.0);
    vec3 ww = normalize(ta - ro);
    vec3 uu = normalize(cross(ww, vec3(0.0, 1.0, 0.0)));
    vec3 vv = cross(uu, ww);
    vec3 rd = normalize(uv.x * uu + uv.y * vv + uZoom * ww);

    const float RS = 0.50;
    const int STEPS = ${steps};

    vec3 pos = ro;
    vec3 vel = rd;
    vec3 col = vec3(0.0);
    float trans = 1.0;
    float closest = 1e5;

    for (int i = 0; i < STEPS; ++i) {
        float r = length(pos);
        closest = min(closest, r);

        if (r < RS) { trans = 0.0; break; }

        float dt = 0.036 * max(r, 0.22);
        vel += -1.50 * RS * pos / (r * r * r * r) * dt;
        vec3 nxt = pos + vel * dt;

        float photon = smoothstep(0.09, 0.0, abs(r - 1.52 * RS));
        col += trans * photon * uAccent * 0.075;

        if (pos.y * nxt.y < 0.0) {
            float f = pos.y / (pos.y - nxt.y + 1e-6);
            vec3 hit = mix(pos, nxt, f);
            float rho = length(hit.xz);
            float inner = RS * 2.45;
            float outer = RS * 11.2;

            if (rho > inner && rho < outer) {
                float x = (rho - inner) / (outer - inner);
                float ang = atan(hit.z, hit.x);
                float spir = 0.5 + 0.5 * sin(2.0 * ang - log(rho + 0.04) * 8.5 - uTime * 1.15);
                float dens = pow(1.0 - x, 1.05) * (0.62 + 0.38 * spir);
                dens *= smoothstep(0.0, 0.08, x) * smoothstep(1.0, 0.72, x);

                vec3 outerC = uDisk * 0.42;
                vec3 midC   = uDisk;
                vec3 hotC   = mix(vec3(1.15, 1.25, 1.40), uAccent, 0.55) * 1.25;
                vec3 dcol = mix(hotC, mix(midC, outerC, smoothstep(0.18, 1.0, x)), smoothstep(0.0, 0.48, x));

                vec3 tang = normalize(vec3(-hit.z, 0.0, hit.x));
                float vkep = 0.50 / sqrt(max(rho, 0.2));
                float dop  = 1.0 + dot(normalize(vel), tang) * vkep * 1.35;
                dcol *= pow(clamp(dop, 0.30, 2.1), 2.4);
                dcol += uAccent * smoothstep(1.12, 1.55, dop) * 0.40;
                dcol *= mix(vec3(1.0), vec3(0.55, 0.40, 0.48), smoothstep(1.0, 0.55, dop));

                col += trans * dcol * dens * 0.72;
                trans *= 1.0 - clamp(dens * 0.55, 0.0, 0.78);
                if (trans < 0.03) break;
            }
        }

        pos = nxt;
        if (r > 22.0) break;
    }

    col += trans * starfield(normalize(vel));

    float ring = smoothstep(0.16, 0.0, abs(closest - 1.5 * RS));
    col += ring * uAccent * 0.28;

    col *= uGrade;
    col = col / (1.0 + col * 0.70);
    col = pow(clamp(col, 0.0, 1.0), vec3(0.92));

    vec2 q = gl_FragCoord.xy / uResolution;
    float vig = pow(16.0 * q.x * q.y * (1.0 - q.x) * (1.0 - q.y), 0.38);
    col *= mix(0.22, 1.0, vig);

    float g = hash12(gl_FragCoord.xy + vec2(fract(uTime) * 73.1, fract(uTime * 0.37) * 19.0));
    col += (g - 0.5) * 0.012;

    fragColor = vec4(col, 1.0);
}`;

function compile(gl, type, source) {
    const sh = gl.createShader(type);
    gl.shaderSource(sh, source);
    gl.compileShader(sh);
    if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) {
        console.warn('hole: shader failed', gl.getShaderInfoLog(sh));
        gl.deleteShader(sh);
        return null;
    }
    return sh;
}

export function startHole(canvas) {
    // Honour the setting before spending anything on it. Somebody who has
    // asked their system for less movement has asked this page too.
    const still = window.matchMedia('(prefers-reduced-motion: reduce)').matches;

    const gl = canvas.getContext('webgl2', {
        alpha: false,
        antialias: false,
        depth: false,
        stencil: false,
        // Was 'low-power', which on a desktop with both a built-in and a real
        // graphics chip is an invitation to draw a hundred-step ray march on
        // the weaker one.
        powerPreference: 'high-performance',
        preserveDrawingBuffer: false,
    });
    // No WebGL2 means no hole. The page stays on its flat background, which is
    // the same near-black the shader clears to, so nothing looks broken.
    if (!gl) return false;

    // A hundred ray marching steps per pixel is a desktop budget. Phones get
    // fewer steps and a smaller buffer; the picture is the same one, drawn
    // more coarsely, which is a fairer trade than a still image.
    const small = Math.min(window.innerWidth, window.innerHeight) < 700;
    const steps = small ? 52 : 84;

    // Every pixel costs a full ray march, so the count is capped rather than
    // left to the display. A maximised window on a 4K screen at 1.5 device
    // pixels is around eight million of them; this is under two and a half,
    // and the canvas is stretched back up by CSS. The picture is a soft glow
    // with no fine detail in it, so the loss is close to invisible and the
    // saving is better than threefold.
    const BUDGET = small ? 900000 : 2400000;

    // Frames are capped too. requestAnimationFrame runs at the refresh rate,
    // and on a 240 Hz monitor that is four times the work for an animation
    // that drifts slowly enough to look identical at sixty.
    const FRAME_MS = 1000 / 60 - 1;

    const vs = compile(gl, gl.VERTEX_SHADER, VERT);
    const fs = compile(gl, gl.FRAGMENT_SHADER, FRAG(steps));
    if (!vs || !fs) return false;

    const prog = gl.createProgram();
    gl.attachShader(prog, vs);
    gl.attachShader(prog, fs);
    gl.linkProgram(prog);
    if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) {
        console.warn('hole: link failed', gl.getProgramInfoLog(prog));
        return false;
    }
    gl.useProgram(prog);

    const uResolution = gl.getUniformLocation(prog, 'uResolution');
    const uTime = gl.getUniformLocation(prog, 'uTime');
    const uPointer = gl.getUniformLocation(prog, 'uPointer');

    // Tighter than the program's 1.22, so the hole fills the page the way it
    // fills the banner. A narrow phone already crops the sides, so it pulls
    // back there instead of showing nothing but shadow.
    gl.uniform1f(gl.getUniformLocation(prog, 'uZoom'), small ? 1.20 : 1.62);

    gl.uniform3fv(gl.getUniformLocation(prog, 'uAccent'), ACCENT);
    gl.uniform3fv(gl.getUniformLocation(prog, 'uDisk'), DISK);
    gl.uniform3fv(gl.getUniformLocation(prog, 'uGrade'), GRADE);

    // The vertex shader builds its own triangle from gl_VertexID, so there is
    // nothing to put in a buffer. WebGL still insists on a bound array object.
    gl.bindVertexArray(gl.createVertexArray());

    let w = 0, h = 0;
    function resize() {
        const scale = Math.min(window.devicePixelRatio || 1, 1);
        let nw = Math.max(1, Math.round(canvas.clientWidth * scale));
        let nh = Math.max(1, Math.round(canvas.clientHeight * scale));

        const over = (nw * nh) / BUDGET;
        if (over > 1) {
            const k = Math.sqrt(over);
            nw = Math.max(1, Math.round(nw / k));
            nh = Math.max(1, Math.round(nh / k));
        }

        if (nw === w && nh === h) return;
        w = canvas.width = nw;
        h = canvas.height = nh;
        gl.viewport(0, 0, w, h);
        gl.uniform2f(uResolution, w, h);
    }

    // The pointer nudges the camera, exactly as uPointer does in the program.
    // Eased, so the hole drifts toward the cursor instead of snapping at it.
    let px = 0, py = 0, tx = 0, ty = 0;
    window.addEventListener('pointermove', (e) => {
        tx = (e.clientX / window.innerWidth) * 2 - 1;
        ty = (e.clientY / window.innerHeight) * 2 - 1;
    }, { passive: true });

    let time = 0;
    let last = performance.now();
    let drawn = 0;
    let running = true;

    function frame(now) {
        if (!running) return;
        requestAnimationFrame(frame);

        if (now - drawn < FRAME_MS) return;
        drawn = now;

        const dt = Math.min((now - last) / 1000, 0.05);
        last = now;
        if (!still) time += dt;

        resize();
        px += (tx - px) * 0.05;
        py += (ty - py) * 0.05;

        gl.uniform1f(uTime, time);
        gl.uniform2f(uPointer, px, py);
        gl.drawArrays(gl.TRIANGLES, 0, 3);
    }

    function stop() { running = false; }
    function start() {
        if (running) return;
        running = true;
        last = drawn = performance.now();
        requestAnimationFrame(frame);
    }

    // Below the hero the veil dims the hole and the reader is reading, so
    // there is no reason to keep ray marching behind their paragraphs. This
    // is what makes scrolling smooth: the page stops competing with itself.
    let heroVisible = true;
    const hero = document.querySelector('header.hero');
    if (hero && 'IntersectionObserver' in window) {
        new IntersectionObserver((entries) => {
            heroVisible = entries[0].isIntersecting;
            if (!heroVisible) stop();
            else if (!document.hidden) start();
        }, { threshold: 0 }).observe(hero);
    }

    // A hidden tab gets no frames from the browser anyway, but the clock would
    // keep running and the hole would jump on return. Stopping the clock too
    // means it carries on from where it was.
    document.addEventListener('visibilitychange', () => {
        if (document.hidden) stop();
        else if (heroVisible) start();
    });

    resize();
    requestAnimationFrame(frame);
    return true;
}
