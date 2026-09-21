// The black hole, drawn the cheap way.
//
// ---------------------------------------------------------------------------
// Why this is not just the program's shader any more
// ---------------------------------------------------------------------------
//
// The first version of this file was src/ui/AuroraWidget.cpp's shader moved
// to WebGL, and it was too slow. Not because the shader is bad - it runs fine
// in the program - but because of what a web page asks of it. The program
// draws into one modest window. A page draws full screen, at whatever refresh
// rate the monitor happens to have, while the browser is also compositing
// scrolling text over the top.
//
// That shader marches a hundred steps along a bent ray for every single
// pixel, every single frame. Capping the frame rate and shrinking the canvas
// helped and was not enough, and the last resort - freezing the animation
// once you scrolled - fixed the numbers by removing the thing people came to
// look at. That was the wrong trade.
//
// So the work is split by how often it actually changes.
//
// **Almost none of it changes.** Where each ray goes, how far it bends, which
// part of the accretion disk it strikes, whether it falls in: all of that is
// decided by the camera and the geometry, and neither moves. It is computed
// once, into textures, and then reused for every frame after.
//
// **What changes is thin.** The spiral pattern turns, the stars twinkle, the
// grain moves. Those are a few lines of arithmetic on numbers already looked
// up from a texture.
//
// The result is the same picture with the ray marching done once instead of
// sixty times a second, so it can run all the way down the page without
// stopping, which is what it should have done in the first place.
//
// ---------------------------------------------------------------------------
// What is stored, and why in that shape
// ---------------------------------------------------------------------------
//
// A ray can cross the disk's plane more than once - that is what draws the
// far side of the disk arcing over the top of the shadow - so two crossings
// are kept. For each one:
//
//   x            how far out the strike is, 0 at the inner edge, 1 at the
//                outer. Negative means this ray missed, and the density
//                curve already returns nothing for that, so there is no flag
//                to keep in step.
//   dop          the Doppler boost, which brightens the side of the disk
//                turning toward the camera and dims the other.
//   cosP, sinP   the spiral's phase, stored as its cosine and sine rather
//                than as an angle.
//
// That last one matters. The textures are sampled with smoothing, and an
// angle wraps from +pi to -pi, so smoothing across that seam would average
// two nearly equal directions into a wrong one and draw a hard line across
// the disk. A cosine and a sine have no seam. The rotation is then exact
// arithmetic rather than a second trigonometric call:
//
//     sin(P - wt) = sinP*cos(wt) - cosP*sin(wt)
//
// The starfield is deliberately *not* baked. It is recomputed each frame from
// the stored escape direction, because that is what keeps the stars twinkling
// and it costs a fraction of what the ray marching did.
//
// ---------------------------------------------------------------------------
// The colours
// ---------------------------------------------------------------------------
//
// These are the ones the shipped theme actually produces, from Theme.cpp.
// Singularity's default seed is #121212, which has no hue at all, and scaling
// a hue-less colour would leave the disk black - so the theme takes a silver
// branch for that case instead. The page can retint at runtime, exactly as
// the program can, which is what the swatches on the page do.
const SILVER = {
    accent: [0.82, 0.90, 1.00],
    disk:   [0.62, 0.78, 1.00],
    grade:  [1.02, 1.06, 1.16],
};

// Theme::storeHole, ported. A seed with no hue keeps the silver disk; any
// other is scaled up until it is bright enough to light one.
export function holeColours(hex) {
    const m = /^#?([0-9a-f]{6})$/i.exec(hex || '');
    if (!m) return SILVER;

    const n = parseInt(m[1], 16);
    let a = [((n >> 16) & 255) / 255, ((n >> 8) & 255) / 255, (n & 255) / 255];

    const max = Math.max(a[0], a[1], a[2]);
    const min = Math.min(a[0], a[1], a[2]);
    if (max - min < 0.02) return SILVER;

    if (max < 0.42) a = a.map(v => v * (0.42 / max));

    return {
        accent: a,
        disk: a.map(v => Math.min(1, Math.max(0.12, v * 0.78))),
        grade: a.map(v => 0.78 + v * 0.38),
    };
}

const VERT = `#version 300 es
void main() {
    vec2 v[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(v[gl_VertexID], 0.0, 1.0);
}`;

// --------------------------------------------------------------------------
// Pass one. Run once, when the size changes. This is the expensive one.
// --------------------------------------------------------------------------
const BAKE = `#version 300 es
precision highp float;

uniform vec2  uResolution;
uniform float uZoom;

layout(location = 0) out vec4 oHitA;
layout(location = 1) out vec4 oHitB;
layout(location = 2) out vec4 oSky;

const float RS = 0.50;
const float INNER = RS * 2.45;
const float OUTER = RS * 11.2;

void main() {
    vec2 uv = (gl_FragCoord.xy - 0.5 * uResolution) / uResolution.y;

    // The camera is fixed. In the program it orbits slowly, which is lovely
    // and is also the one thing that would force all of this to be computed
    // again every frame. The drift you see on the page is the whole picture
    // being sampled with a moving offset instead, which is why this is baked
    // a little wider than it is shown.
    vec3 ro = vec3(0.0, 0.62, 7.15);
    vec3 ta = vec3(0.0, 0.02, 0.0);
    vec3 ww = normalize(ta - ro);
    vec3 uu = normalize(cross(ww, vec3(0.0, 1.0, 0.0)));
    vec3 vv = cross(uu, ww);
    vec3 rd = normalize(uv.x * uu + uv.y * vv + uZoom * ww);

    vec3 pos = ro;
    vec3 vel = rd;

    vec4 hitA = vec4(-1.0, 1.0, 0.0, 0.0);
    vec4 hitB = vec4(-1.0, 1.0, 0.0, 0.0);
    int found = 0;

    float photon = 0.0;
    bool captured = false;

    for (int i = 0; i < 100; ++i) {
        float r = length(pos);

        if (r < RS) { captured = true; break; }

        float dt = 0.036 * max(r, 0.22);
        vel += -1.50 * RS * pos / (r * r * r * r) * dt;
        vec3 nxt = pos + vel * dt;

        photon += smoothstep(0.09, 0.0, abs(r - 1.52 * RS)) * 0.075;

        if (pos.y * nxt.y < 0.0) {
            float f = pos.y / (pos.y - nxt.y + 1e-6);
            vec3 hit = mix(pos, nxt, f);
            float rho = length(hit.xz);

            if (rho > INNER && rho < OUTER) {
                float x = (rho - INNER) / (OUTER - INNER);
                float ang = atan(hit.z, hit.x);
                float phase = 2.0 * ang - log(rho + 0.04) * 8.5;

                vec3 tang = normalize(vec3(-hit.z, 0.0, hit.x));
                float vkep = 0.50 / sqrt(max(rho, 0.2));
                float dop = 1.0 + dot(normalize(vel), tang) * vkep * 1.35;

                vec4 rec = vec4(x, dop, cos(phase), sin(phase));
                if (found == 0) { hitA = rec; found = 1; }
                else if (found == 1) { hitB = rec; found = 2; }
            }
        }

        pos = nxt;
        if (r > 22.0) break;
    }

    oHitA = hitA;
    oHitB = hitB;
    // A captured ray sees no sky at all. Storing a zero direction says so
    // without spending a channel on a flag.
    oSky = vec4(captured ? vec3(0.0) : normalize(vel), photon);
}`;

// --------------------------------------------------------------------------
// Pass two. Run every frame. This is the cheap one.
// --------------------------------------------------------------------------
const DRAW = `#version 300 es
precision highp float;

uniform sampler2D tHitA;
uniform sampler2D tHitB;
uniform sampler2D tSky;

uniform vec2  uResolution;
uniform float uTime;
uniform vec2  uDrift;
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
                vec3 sp = mix(vec3(0.75, 0.88, 1.0), uAccent, 0.4);
                col += sp * 0.35 * exp(-abs(f.x) * 55.0) * exp(-abs(f.y) * 10.0);
                col += sp * 0.18 * exp(-abs(f.y) * 55.0) * exp(-abs(f.x) * 10.0);
            }
        }
    }
    return col;
}

// One crossing of the disk, coloured for this instant. Everything here came
// out of a texture except the time, which is the whole point.
void disk(vec4 rec, float wt, inout vec3 col, inout float trans) {
    float x = rec.x;
    if (x < -0.5) return;

    // sin(phase - wt), rebuilt from the stored cosine and sine.
    float s = rec.w * cos(wt) - rec.z * sin(wt);
    float spir = 0.5 + 0.5 * s;

    float dens = pow(max(1.0 - x, 0.0), 1.05) * (0.62 + 0.38 * spir);
    dens *= smoothstep(0.0, 0.08, x) * smoothstep(1.0, 0.72, x);
    if (dens <= 0.0) return;

    float dop = rec.y;
    vec3 outerC = uDisk * 0.42;
    vec3 midC   = uDisk;
    vec3 hotC   = mix(vec3(1.15, 1.25, 1.40), uAccent, 0.55) * 1.25;
    vec3 dcol = mix(hotC, mix(midC, outerC, smoothstep(0.18, 1.0, x)), smoothstep(0.0, 0.48, x));

    dcol *= pow(clamp(dop, 0.30, 2.1), 2.4);
    dcol += uAccent * smoothstep(1.12, 1.55, dop) * 0.40;
    dcol *= mix(vec3(1.0), vec3(0.55, 0.40, 0.48), smoothstep(1.0, 0.55, dop));

    col += trans * dcol * dens * 0.72;
    trans *= 1.0 - clamp(dens * 0.55, 0.0, 0.78);
}

void main() {
    vec2 q = gl_FragCoord.xy / uResolution;

    // The baked picture is wider than what is shown, and this slides the
    // window over it. That is the slow drift, and the lean toward the cursor.
    vec2 suv = clamp((q - 0.5) * 0.90 + 0.5 + uDrift, vec2(0.001), vec2(0.999));

    vec4 a   = texture(tHitA, suv);
    vec4 b   = texture(tHitB, suv);
    vec4 sky = texture(tSky, suv);

    float wt = uTime * 1.15;
    vec3 col = sky.w * uAccent;
    float trans = 1.0;

    disk(a, wt, col, trans);
    disk(b, wt, col, trans);

    // A zero direction is a ray that fell in, and sees nothing.
    if (dot(sky.xyz, sky.xyz) > 0.25)
        col += trans * starfield(normalize(sky.xyz));

    col *= uGrade;
    col = col / (1.0 + col * 0.70);
    col = pow(clamp(col, 0.0, 1.0), vec3(0.92));

    float vig = pow(16.0 * q.x * q.y * (1.0 - q.x) * (1.0 - q.y), 0.38);
    col *= mix(0.22, 1.0, vig);

    float g = hash12(gl_FragCoord.xy + vec2(fract(uTime) * 73.1, fract(uTime * 0.37) * 19.0));
    col += (g - 0.5) * 0.012;

    fragColor = vec4(col, 1.0);
}`;

function build(gl, fragSource) {
    const mk = (type, src) => {
        const sh = gl.createShader(type);
        gl.shaderSource(sh, src);
        gl.compileShader(sh);
        if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) {
            console.warn('hole:', gl.getShaderInfoLog(sh));
            return null;
        }
        return sh;
    };

    const vs = mk(gl.VERTEX_SHADER, VERT);
    const fs = mk(gl.FRAGMENT_SHADER, fragSource);
    if (!vs || !fs) return null;

    const p = gl.createProgram();
    gl.attachShader(p, vs);
    gl.attachShader(p, fs);
    gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) {
        console.warn('hole:', gl.getProgramInfoLog(p));
        return null;
    }
    return p;
}

export function startHole(canvas) {
    const gl = canvas.getContext('webgl2', {
        alpha: false,
        antialias: false,
        depth: false,
        stencil: false,
        powerPreference: 'high-performance',
        preserveDrawingBuffer: false,
    });
    if (!gl) return null;

    // Rendering into a float texture is what makes the baking possible. It is
    // everywhere now, but a browser without it gets the drawn banner rather
    // than a half working picture.
    if (!gl.getExtension('EXT_color_buffer_float')) return null;

    const still = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
    const small = Math.min(window.innerWidth, window.innerHeight) < 700;

    const bakeProg = build(gl, BAKE);
    const drawProg = build(gl, DRAW);
    if (!bakeProg || !drawProg) return null;

    gl.bindVertexArray(gl.createVertexArray());

    const bakeU = {
        res: gl.getUniformLocation(bakeProg, 'uResolution'),
        zoom: gl.getUniformLocation(bakeProg, 'uZoom'),
    };
    const drawU = {
        res: gl.getUniformLocation(drawProg, 'uResolution'),
        time: gl.getUniformLocation(drawProg, 'uTime'),
        drift: gl.getUniformLocation(drawProg, 'uDrift'),
        accent: gl.getUniformLocation(drawProg, 'uAccent'),
        disk: gl.getUniformLocation(drawProg, 'uDisk'),
        grade: gl.getUniformLocation(drawProg, 'uGrade'),
    };

    gl.useProgram(drawProg);
    gl.uniform1i(gl.getUniformLocation(drawProg, 'tHitA'), 0);
    gl.uniform1i(gl.getUniformLocation(drawProg, 'tHitB'), 1);
    gl.uniform1i(gl.getUniformLocation(drawProg, 'tSky'), 2);

    let fbo = null;
    const tex = [null, null, null];
    let bakeW = 0, bakeH = 0;

    function makeTargets(w, h) {
        for (let i = 0; i < 3; ++i) {
            if (tex[i]) gl.deleteTexture(tex[i]);
            tex[i] = gl.createTexture();
            gl.bindTexture(gl.TEXTURE_2D, tex[i]);
            gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA16F, w, h, 0, gl.RGBA, gl.HALF_FLOAT, null);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
        }

        if (fbo) gl.deleteFramebuffer(fbo);
        fbo = gl.createFramebuffer();
        gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
        for (let i = 0; i < 3; ++i) {
            gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0 + i, gl.TEXTURE_2D, tex[i], 0);
        }
        gl.drawBuffers([gl.COLOR_ATTACHMENT0, gl.COLOR_ATTACHMENT1, gl.COLOR_ATTACHMENT2]);

        const ok = gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE;
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        return ok;
    }

    function bake() {
        gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
        gl.viewport(0, 0, bakeW, bakeH);
        gl.useProgram(bakeProg);
        gl.uniform2f(bakeU.res, bakeW, bakeH);
        // Baked wider than shown, leaving the margin the drift slides across.
        gl.uniform1f(bakeU.zoom, (small ? 1.20 : 1.62) * 0.90);
        gl.drawArrays(gl.TRIANGLES, 0, 3);
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    }

    // Three floating point textures at full size is a lot of memory for a web
    // page, and the smoothing on the way out hides the difference, so they
    // are baked smaller than they are shown.
    const BAKE_BUDGET = small ? 700000 : 1600000;
    const DRAW_BUDGET = small ? 1100000 : 2600000;

    function fit(cssW, cssH, budget) {
        const s = Math.min(window.devicePixelRatio || 1, 1.25);
        let w = Math.max(1, Math.round(cssW * s));
        let h = Math.max(1, Math.round(cssH * s));
        const over = (w * h) / budget;
        if (over > 1) {
            const k = Math.sqrt(over);
            w = Math.max(1, Math.round(w / k));
            h = Math.max(1, Math.round(h / k));
        }
        return [w, h];
    }

    let ok = true;
    function resize() {
        const [dw, dh] = fit(canvas.clientWidth, canvas.clientHeight, DRAW_BUDGET);
        if (dw === canvas.width && dh === canvas.height) return;

        canvas.width = dw;
        canvas.height = dh;

        const [bw, bh] = fit(canvas.clientWidth, canvas.clientHeight, BAKE_BUDGET);
        bakeW = bw;
        bakeH = bh;

        ok = makeTargets(bakeW, bakeH);
        if (ok) bake();
    }

    for (let i = 0; i < 3; ++i) {
        gl.activeTexture(gl.TEXTURE0 + i);
        gl.bindTexture(gl.TEXTURE_2D, tex[i]);
    }

    let colours = SILVER;
    let tx = 0, ty = 0, px = 0, py = 0;

    window.addEventListener('pointermove', (e) => {
        tx = (e.clientX / window.innerWidth) * 2 - 1;
        ty = (e.clientY / window.innerHeight) * 2 - 1;
    }, { passive: true });

    let time = 0;
    let last = performance.now();
    let drawnAt = 0;
    let alive = true;

    // Frames are capped. requestAnimationFrame runs at the display's refresh
    // rate, and a 240 Hz monitor would otherwise ask for four times the work
    // to show an animation that looks identical at sixty.
    const FRAME_MS = 1000 / 60 - 1.5;

    function frame(now) {
        if (!alive) return;
        requestAnimationFrame(frame);
        if (now - drawnAt < FRAME_MS) return;
        drawnAt = now;

        const dt = Math.min((now - last) / 1000, 0.05);
        last = now;
        if (!still) time += dt;

        resize();
        if (!ok) return;

        px += (tx - px) * 0.04;
        py += (ty - py) * 0.04;

        for (let i = 0; i < 3; ++i) {
            gl.activeTexture(gl.TEXTURE0 + i);
            gl.bindTexture(gl.TEXTURE_2D, tex[i]);
        }

        gl.viewport(0, 0, canvas.width, canvas.height);
        gl.useProgram(drawProg);
        gl.uniform2f(drawU.res, canvas.width, canvas.height);
        gl.uniform1f(drawU.time, time);
        gl.uniform2f(drawU.drift,
            Math.sin(time * 0.061) * 0.030 + px * 0.016,
            Math.cos(time * 0.047) * 0.022 - py * 0.012);
        gl.uniform3fv(drawU.accent, colours.accent);
        gl.uniform3fv(drawU.disk, colours.disk);
        gl.uniform3fv(drawU.grade, colours.grade);
        gl.drawArrays(gl.TRIANGLES, 0, 3);
    }

    // Only a hidden tab stops it. Scrolling does not: an animated background
    // that freezes the moment you read something is worse than no animation,
    // and now that a frame is cheap there is no reason to.
    document.addEventListener('visibilitychange', () => {
        if (document.hidden) {
            alive = false;
        } else if (!alive) {
            alive = true;
            last = drawnAt = performance.now();
            requestAnimationFrame(frame);
        }
    });

    resize();
    if (!ok) return null;
    requestAnimationFrame(frame);

    return {
        setSeed(hex) { colours = holeColours(hex); },
    };
}
