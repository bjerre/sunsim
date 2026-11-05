//! Turns what the lights should show into the ZCL commands that get them there.

use ev::*;

/// The commands that take a light with `caps` from `sent` (None: unknown,
/// send everything) to `to`: only what changed and the light takes (a plug:
/// On/Off alone); None if that's nothing. Its address is left for the
/// caller.
pub fn commands_for(sent: Option<&LightState>, to: &LightChange, caps: Caps) -> Option<SetLight> {
    let mut s = SetLight::default();
    let was_on = sent.is_some_and(|s| s.on);
    if to.state.on {
        if !was_on {
            s.power = Some(true);
        }
        if caps.level && (!was_on || sent.is_some_and(|s| s.level != to.state.level)) {
            s.level = Some(to.state.level);
            s.level_transition = to.level_transition;
        }
        if caps.color && (!was_on || sent.is_some_and(|s| s.color != to.state.color)) {
            s.color = Some(to.state.color);
            s.color_transition = to.color_transition;
        }
    } else if sent.is_none() || was_on {
        s.power = Some(false);
    }
    if s.power.is_none() && s.level.is_none() && s.color.is_none() {
        return None;
    }
    Some(s)
}

#[derive(Clone, Copy, Debug)]
struct Light {
    known: bool, // paired with a known endpoint
    short_addr: u16,
    endpoint: u8,
    reachable: bool,
    sent: Option<LightState>, // what we last told it; None = unknown, resend everything
}

impl Default for Light {
    fn default() -> Self {
        Light { known: false, short_addr: 0, endpoint: 0, reachable: true, sent: None }
    }
}

/// Turns what the lights *should* show (OutputTarget: the running program's,
/// or the weather's, see Overlays) into the ZCL commands that get them there
/// (SetLight), as much as each light's kind takes (caps). Only what changed
/// since the last command is sent: On/Off on a change, level and color when
/// they move. Lights marked unreachable get nothing until they answer again
/// (the registry then posts RefreshLights). Their addresses come from
/// RoleAssigned/RoleCleared.
pub struct LightOutput {
    post: Post,
    lights: PerRole<Light>, // lights only
}

impl LightOutput {
    pub fn new(post: Post) -> Self {
        LightOutput { post, lights: PerRole::default() }
    }

    fn role_assigned(&mut self, e: &RoleAssigned) {
        if !is_light(e.role) {
            return;
        }
        let l = &mut self.lights[e.role];
        l.known = e.record.valid && e.record.endpoint != 0;
        l.short_addr = e.record.short_addr;
        l.endpoint = e.record.endpoint;
        l.sent = None; // (re)paired or moved: resend everything next time
    }

    fn role_cleared(&mut self, e: &RoleCleared) {
        if is_light(e.role) {
            self.lights[e.role] = Light::default();
        }
    }

    fn reachability_changed(&mut self, e: &ReachabilityChanged) {
        if !is_light(e.role) {
            return;
        }
        let l = &mut self.lights[e.role];
        l.reachable = e.reachable;
        if !e.reachable {
            l.sent = None; // unknown state when it's back: resend everything
        }
    }

    fn output_target(&mut self, e: &OutputTarget) {
        if !is_light(e.role) {
            return;
        }
        let l = &mut self.lights[e.role];
        if !(l.known && l.reachable) {
            return;
        }
        if let Some(mut s) = commands_for(l.sent.as_ref(), e, caps(e.role)) {
            s.role = e.role;
            s.short_addr = l.short_addr;
            s.endpoint = l.endpoint;
            self.post.send(s);
        }
        l.sent = Some(e.state);
    }
}

impl Module for LightOutput {
    fn on(&mut self, e: &Event) {
        match e {
            Event::RoleAssigned(e) => self.role_assigned(e),
            Event::RoleCleared(e) => self.role_cleared(e),
            Event::ReachabilityChanged(e) => self.reachability_changed(e),
            Event::OutputTarget(e) => self.output_target(e),
            _ => {}
        }
    }
}
