// ============================================================================
// TinyEReader (CrowPanel ESP32 E-Paper HMI 2.13") battery case
// ============================================================================
//
// Two-piece clamshell, no fasteners: a front frame (holds the board+panel
// from the front, open at the back) and a back cover (battery bay + button
// levers) that press-fits onto it with 6 printed cantilever snap clips.
// A third, optional small part -- a grip clip for the K5 rotary wheel --
// is not part of the clamshell at all; see "Dial grip cap" below.
//
// COORDINATE CONVENTION: X/Y are board-local (origin at the PCB's
// bottom-left corner, same as Elecrow's own Eagle file). Z=0 is the PCB
// plane (the seam where the two halves meet) for BOTH pieces -- the front
// frame occupies Z<0 (toward the display), the back cover occupies Z>0
// (toward the battery). Every feature below is placed with an absolute Z
// in that shared frame, not offsets relative to some other feature, so
// it's harder for a translate() to silently double up.
//
// WHAT'S REAL vs ESTIMATED
// -------------------------
// Board outline and every connector/button XY position below came from
// Elecrow's own Eagle PCB file (CrowPanel ESP32 Display-2.13(E) Inch(1).brd,
// github.com/Elecrow-RD/CrowPanel-ESP32-2.13-E-paper-HMI-Display-with-122-250),
// cross-checked against their "hardware overview" product photo. Those
// numbers are trustworthy.
//
// Z-heights (component clearance, standoff height, tact-switch actuation
// point) are NOT from a verified source -- Eagle board files are 2D, and
// Elecrow doesn't publish a mechanical/Z drawing. Every such value is
// called out in the "VERIFY WITH CALIPERS" block below with what it means
// and a reasonable starting guess. Print a single test wall with one snap
// clip and one button lever before committing to a full print -- much
// cheaper than finding out the fit is off after a multi-hour print.
//
// !!! BATTERY CONNECTOR MISMATCH -- READ THIS FIRST !!!
// The board's BAT input is a JST SH 1.0mm-pitch 2-pin socket (confirmed
// from Elecrow's own schematic silkscreen, labeled "BAT-1.0MM-W", and
// their wiki page). The EasyLander battery linked uses a JST PH 2.0mm-pitch
// connector. These do not mate -- different pitch, physically won't plug
// in. You'll need one of:
//   - the same cell with an SH1.0 connector instead (common on other
//     listings for the same physical battery size)
//   - a JST-PH-to-JST-SH adapter/pigtail
//   - re-terminate the wire yourself (cut the PH connector off, crimp or
//     solder on an SH1.0 one, or solder straight to the board's BAT pads)
// This doesn't affect the case dimensions below -- the cell itself
// (50x15x10mm) is unaffected either way -- just the wiring.
//
// Print: PLA, 2+ perimeters, 15%+ infill. Both clamshell halves print
// face-down (flat on the bed, as laid out in the render section) -- no
// supports needed for either. The dial grip cap (3rd part) also prints
// with no supports, arc-side down.

/* [Board geometry -- REAL, from Elecrow's Eagle file] */
board_w = 63.20;          // PCB outline X extent
board_h = 31.20;          // PCB outline Y extent
corner_chamfer = 1.0;     // 45-degree corner cut, all 4 corners (matches the real outline)

// All positions below are board-local XY (origin = bottom-left of the PCB
// bounding box). This is the BACK of the board (the populated/component
// face with the buttons -- what Elecrow's "hardware overview" photo shows,
// and what this case's back cover faces).
usb_c_x   = 32.19;   // USB-C, centered on the Y=0 (bottom) edge -- the only
                      // opening besides the display and the left-edge
                      // buttons/dial. BOOT, RESET, GPIO_D, and UART0 are all
                      // sealed under solid wall -- not needed day to day,
                      // and BOOT/RESET are hardwired (bootloader/reset only,
                      // not reconfigurable), so there's nothing lost by
                      // covering them.

menu_y = 25.69;   // K3 MENU -- side-actuated tact switch, on the X=0 (left) edge
exit_y = 3.41;    // K4 EXIT -- side-actuated tact switch, on the X=0 (left) edge
dial_y = 14.60;   // K5 rotary wheel (Eagle package TM_2024A, real element center
                  // at x=7.04,y=14.60 -- the Y here is exact, from the same
                  // Eagle file as everything else). It's a round knurled wheel
                  // mounted flush near the left edge, spinning in the plane of
                  // the board (per Elecrow's product photo) -- you roll it with
                  // a fingertip through a window in the left wall, same idea as
                  // MENU/EXIT's edge access. The window alone is real geometry;
                  // the separate grip cap in the "Dial grip cap" section below
                  // is not -- see that section for what's guessed there.

/* [Battery -- from the AliExpress listing] */
bat_l = 50; bat_w = 15; bat_h_cell = 10;
bat_slack = 0.6;   // extra room per side so it slides in without forcing

/* [VERIFY WITH CALIPERS on your actual board before printing the full case] */
pcb_thickness   = 1.6;  // standard for a board this size, but confirm
front_clearance = 4.5;  // depth from the front bezel lip to the PCB plane: the
                         // panel itself, its foam/adhesive standoff, and the
                         // folded FPC ribbon on the left edge all live here.
                         // Guess based on typical thin e-paper module stacks --
                         // measure your actual assembled unit's front-to-PCB depth.
back_clearance  = 4.0;  // tallest thing on the populated face: the ESP32-S3
                         // module, tact switch bodies, USB-C connector height.
                         // Guess from typical QFN module + connector heights --
                         // measure the tallest component on your board.
switch_nub_z    = 1.8;  // height above the PCB plane where a side-tact
                         // switch's actuation nub sits (K3/K4) -- a guess for
                         // a KEY_3x6x3.5-style switch; verify against the
                         // actual part if you can read its markings

wall = 1.8;                  // case wall thickness
board_fit_clearance = 0.3;   // slack per side in the board-retention slot

/* [Snap-fit clips -- conservative proportions for PLA, not aggressive] */
clip_w      = 4;     // width along the wall
clip_len    = 6;      // how far it reaches from the seam into the other half
clip_t      = 1.3;    // arm thickness
clip_catch  = 0.6;    // how far the catch tooth sticks out sideways

$fn = 64;

// ---------------------------------------------------------------------------
// Derived
// ---------------------------------------------------------------------------
back_total_depth = back_clearance + bat_h_cell + wall;
bezel_inset = 3.0;    // how far the front opening is inset from the board edge, hiding it
shelf_depth = 1.5;    // thickness of the shelf the board rests on, measured from the front

// 2D outline of the PCB (and by extension the case, offset from this) --
// the real chamfered-rectangle shape from the Eagle "Dimension" layer. The
// board also has a small notch cut into the right edge (a manufacturing/
// routing detail, not load-bearing) that's left out here for simplicity --
// harmless to omit, it just means the case is a hair more material there
// than the real PCB silhouette.
module board_outline_2d() {
  polygon(points = [
    [0, corner_chamfer],
    [corner_chamfer, 0],
    [board_w - corner_chamfer, 0],
    [board_w, corner_chamfer],
    [board_w, board_h - corner_chamfer],
    [board_w - corner_chamfer, board_h],
    [corner_chamfer, board_h],
    [0, board_h - corner_chamfer],
  ]);
}

module wall_ring_2d() {
  difference() {
    offset(delta = wall) board_outline_2d();
    board_outline_2d();
  }
}

// ---------------------------------------------------------------------------
// Front frame: Z from -front_clearance (bezel face) to 0 (seam). A bezel
// lip at the very front hides the board edge and holds the panel from
// pushing forward; a shelf partway back is what the board actually rests
// its face against; side walls run the rest of the way back to the seam.
// ---------------------------------------------------------------------------
module front_frame() {
  difference() {
    union() {
      // Side walls, bezel face back to the seam
      translate([0, 0, -front_clearance])
        linear_extrude(front_clearance)
          wall_ring_2d();
      // Bezel lip (closes the front partway, leaving the display opening)
      translate([0, 0, -front_clearance])
        linear_extrude(shelf_depth)
          difference() {
            offset(delta = wall) board_outline_2d();
            offset(delta = -bezel_inset) board_outline_2d();
          }
    }
    // Snap-clip catch pockets, cut into the inside face of the wall right
    // at the seam -- the back cover's clip_arm() tips land here.
    for (p = clip_specs()) clip_socket(p);
  }
}

// ---------------------------------------------------------------------------
// Back cover: Z from 0 (seam) to back_total_depth (rear face). Everything
// on the populated face lives here -- the USB-C cutout, MENU/EXIT levers,
// the dial window, and the battery bay. BOOT, RESET, GPIO_D, and UART0 are
// deliberately left sealed under solid wall (see usb_c_x comment above).
// ---------------------------------------------------------------------------
module back_cover() {
  union() {
    // Everything solid, WITH the lever pockets, USB-C slot, dial window, and
    // battery bay cut out of it. The lever paddles are deliberately NOT part
    // of this union -- if they were unioned in before the lever_pocket() cut
    // (as they used to be), the pocket (sized larger than the paddle so it
    // can flex) would subtract the paddle right along with the material
    // around it, leaving nothing there. They're added back below, after the
    // cut, so they survive as real material connected only through the
    // hinge.
    difference() {
      union() {
        // Side walls, seam back through the battery bay
        linear_extrude(back_total_depth) wall_ring_2d();
        // Rear face (closes the battery bay)
        translate([0, 0, back_total_depth - wall])
          linear_extrude(wall) offset(delta = wall) board_outline_2d();
        // Snap clip arms
        for (p = clip_specs()) clip_arm(p);
      }

      // --- edge cutouts (board-local XY, absolute Z) ---
      // USB-C, bottom edge -- the only cutout besides the display opening and
      // the left-edge dial/lever pockets. BOOT, RESET, GPIO_D, and UART0 are
      // all left under solid wall on purpose (see usb_c_x comment above).
      translate([usb_c_x - 5, -wall - 1, back_clearance - 4])
        cube([10, wall + 2, 6]);
      // Dial window, left edge -- generously sized, no lever, direct access
      translate([-wall - 1, dial_y - 7.5, back_clearance - 6])
        cube([wall + 2, 15, 12]);
      // MENU / EXIT lever pockets: free the paddle on 3 sides, matching
      // menu_exit_lever()'s own geometry (hinge stays uncut, at the top)
      lever_pocket(menu_y);
      lever_pocket(exit_y);
      // Battery bay
      battery_bay_cut();
    }

    // MENU / EXIT cantilever paddles (see menu_exit_lever()) -- added after
    // the cut above, so the pocket that frees them doesn't also delete them.
    menu_exit_lever(menu_y);
    menu_exit_lever(exit_y);
  }
}

// Cantilever paddle for a side-actuated switch (K3 MENU / K4 EXIT), which
// sit right at the board's X=0 edge with their actuation nub facing +X
// (into the board). The paddle needs to be reachable from OUTSIDE the
// case, i.e. at X < -wall, and pivot so pressing it in the -X->+X
// direction (a normal "push this edge button" motion) drives a nub on its
// inner end into the switch just past X=0.
//
// It's built as an L-shape in the XZ plane: a hinge strip flush with the
// wall's own thickness (X from -wall to 0) at the BOTTOM of its Z-span,
// and a paddle body above that, spanning from outside the wall's outer
// face (X = -wall-paddle_out) in to the wall's inner face (X=0). Pressing
// the paddle's outer face flexes the hinge and swings the whole body
// slightly in +X, and the nub at its inner-top corner pokes past X=0 to
// reach the switch.
paddle_w = 7;         // along the wall (Y span)
paddle_span_z = 9;    // how tall the moving part is (Z span)
paddle_out = 3;       // how far the paddle sticks out past the wall's outer face
hinge_t = 0.7;         // flexure thickness

module menu_exit_lever(y_center) {
  z0 = 0;  // bottom of the pocket/paddle -- pinned to the seam (Z=0) so the
           // whole mechanism stays in the back cover's own Z>=0 territory
           // and can't overlap the front frame's solid wall on the other
           // side of the seam
  // hinge: thin strip across the wall's own thickness, at the bottom of the span
  translate([-wall, y_center - paddle_w / 2, z0])
    cube([wall, paddle_w, hinge_t]);
  // paddle body: outside the wall's outer face, up to its inner face, above the hinge
  translate([-wall - paddle_out, y_center - paddle_w / 2, z0 + hinge_t])
    cube([wall + paddle_out, paddle_w, paddle_span_z - hinge_t]);
  // press nub: pokes just past the wall's inner face (X=0) at the switch's
  // estimated actuation height, so deflecting the paddle presses it
  translate([-0.3, y_center, z0 + hinge_t + switch_nub_z])
    rotate([0, 90, 0]) cylinder(h = 1.5, d = 1.6);
}

// Frees the paddle body (not the hinge) on all sides so it can flex --
// same span as menu_exit_lever()'s paddle body, oversized slightly for
// print clearance.
module lever_pocket(y_center) {
  z0 = 0;  // must match menu_exit_lever()'s z0
  translate([-wall - paddle_out - 0.3, y_center - paddle_w / 2 - 0.3, z0 + hinge_t - 0.3])
    cube([wall + paddle_out + 0.6, paddle_w + 0.6, paddle_span_z - hinge_t + 0.6]);
}

// ---------------------------------------------------------------------------
// Snap-fit clips: 6 cantilever arms on the back cover (growing from Z=0
// into the front frame's Z<0 territory), catching into pockets cut in the
// front frame's wall. Each entry is [x, y, dir] where dir is a unit 2D
// vector pointing OUT of the case at that point (the direction the catch
// tooth juts toward).
//
// The left edge (X=0) gets none: the dial window and MENU/EXIT lever
// pockets already consume Y~=0 to Y~=29.2 of the board's 31.2mm height,
// leaving no clear run of wall long enough for a 4mm-wide clip -- an
// earlier version put one at Y=board_h*0.5 anyway, which landed inside the
// dial window and got carved away by that cutout, silently leaving that
// side unclipped. The right edge (X=board_w), directly opposite the
// buttons, is completely clear and previously had no clips at all, which
// is what let that side sit open -- it gets 2 here to make up for the side
// that can't have any.
// ---------------------------------------------------------------------------
function clip_specs() = [
  [board_w * 0.25, 0, [0, -1]],
  [board_w * 0.75, 0, [0, -1]],
  [board_w * 0.3, board_h, [0, 1]],
  [board_w * 0.7, board_h, [0, 1]],
  [board_w, board_h * 0.3, [1, 0]],
  [board_w, board_h * 0.7, [1, 0]],
];

module clip_arm(spec) {
  x = spec[0]; y = spec[1]; dir = spec[2];
  // local frame: origin at the wall's outer face at (x,y), z=0 = seam,
  // arm reaches toward -z (into the front frame), tooth points along `dir`
  translate([x, y, 0])
    rotate([0, 0, atan2(dir[1], dir[0])])
      translate([-clip_t / 2, 0, -clip_len])
        union() {
          cube([clip_t, clip_w, clip_len]);
          translate([clip_t, -0.5, clip_len - 2]) cube([clip_catch, clip_w + 1, 2]);
        }
}

module clip_socket(spec) {
  x = spec[0]; y = spec[1]; dir = spec[2];
  translate([x, y, 0])
    rotate([0, 0, atan2(dir[1], dir[0])])
      translate([-clip_t / 2 - 0.3, -0.8, -clip_len - 0.3])
        cube([clip_t + clip_catch + 0.6, clip_w + 1.6, 2.6]);
}

// ---------------------------------------------------------------------------
// Battery bay: pocket in the back cover, sized for the cell plus slack,
// positioned in the open middle band of the board -- clear of GPIO_D (top
// edge) and UART0/USB-C (bottom edge), which is what "between GPIO_D and
// UART0" comes out to once you plot their real Y positions.
// ---------------------------------------------------------------------------
module battery_bay_cut() {
  bay_w = bat_l + 2 * bat_slack;
  bay_h = bat_w + 2 * bat_slack;
  bay_x = (board_w - bay_w) / 2;
  bay_y = (board_h - bay_h) / 2;  // centered vertically -- clears both headers with room either side
  translate([bay_x, bay_y, back_clearance])
    cube([bay_w, bay_h, bat_h_cell + bat_slack + wall]);
}

// ---------------------------------------------------------------------------
// Dial grip cap -- OPTIONAL 3rd part, NOT fused to the clamshell, NOT
// snap-fit to anything printed. It's a small open C-channel that slides
// radially onto the exposed arc of the K5 wheel (the part already sticking
// past the board edge / visible through the dial window above) and grips
// it by friction between two thin lips, one over the wheel's front face
// and one over its back -- like a snap-on shoe. No glue, nothing touches a
// solder joint, fully reversible.
//
// UNLIKE THE REST OF THIS FILE, THE WHEEL'S OWN DIMENSIONS HERE ARE A
// GUESS. Eagle's package TM_2024A only has 2D pad/courtyard data -- there's
// no mechanical drawing for the wheel's actual molded shape, and no
// datasheet on hand for it. dial_wheel_dia/thick below are read off the
// courtyard size and the product photo, not measured. Print this part
// ALONE first (it's small and fast) and test-fit it gently on the real
// wheel before trusting it:
//   - too tight: it just won't push on -- doesn't stress the switch unless
//     you force it. Bump dial_wheel_dia/thick up 0.2-0.3mm and reprint.
//   - too loose: it'll spin free without gripping. Reduce the same amounts.
// If it never wants to grip well, going back to bare/rawdog access (which
// the plain window already gives you, cap or no cap) is a completely fine
// outcome -- this piece is a nice-to-have, not load-bearing for the case.
/* [Dial grip cap -- GUESSED, verify on the real part before trusting] */
dial_wheel_dia   = 10;    // guessed wheel OD
dial_wheel_thick = 4;     // guessed wheel thickness (its Z extent)
dial_lip_reach   = 1.5;   // how far the top/bottom lips overhang onto the wheel's faces
dial_cap_wall    = 1.4;   // radial bridge thickness -- the part you press/roll
dial_cap_arc     = 90;    // degrees of the wheel's circumference this clip covers

module dial_grip_cap() {
  r_in  = dial_wheel_dia / 2 - dial_lip_reach;
  r_out = dial_wheel_dia / 2 + dial_cap_wall;
  lip_t = 1.2;
  gap   = dial_wheel_thick - 0.3;   // slightly undersized -- friction fit
  total_h = 2 * lip_t + gap;

  difference() {
    rotate_extrude(angle = dial_cap_arc)
      translate([r_in, 0])
        difference() {
          square([r_out - r_in, total_h]);
          translate([0, lip_t]) square([r_out - r_in - dial_cap_wall, gap]);
        }
    // shallow grip grooves across the outer (pressing) face
    for (a = [10 : 15 : dial_cap_arc - 10])
      rotate([0, 0, a])
        translate([r_out - 0.5, -0.4, -1])
          cube([1, 0.8, total_h + 2]);
  }
}

// ---------------------------------------------------------------------------
// Render -- both clamshell pieces oriented print-side-down (flat on the
// bed, as OpenSCAD/a slicer would want them), laid out side by side for
// viewing, plus the dial grip cap off to the side (its case-relative X/Y
// placement here is just for looking at it together with the rest -- it
// doesn't mount to the case, it mounts to the wheel directly).
// ---------------------------------------------------------------------------
translate([0, 0, front_clearance]) rotate([180, 0, 0]) front_frame();

translate([board_w + 15, 0, 0]) back_cover();

translate([board_w + 15, board_h + 15, 0]) dial_grip_cap();
