/** Browser-only controller. All credentials go directly to the Clerk SDK.
 * No passwords, invitations or provider tokens are persisted by this app. */
export async function bootAuth(config) {
  const $ = (id) => document.getElementById(id);
  const status = $('status');
  const say = (text) => {
    status.textContent = text;
  };
  let busy = false;
  let clerk;
  let verification = null;
  const incoming = new URL(location.href);
  const allowedReturns = [
    '/',
    '/downloads/',
    '/entertainment/',
    '/contact/',
    '/privacy/',
    '/legal/',
    '/__agreements',
  ];
  const requestedReturn = incoming.searchParams.get('returnTo');
  const returnTo = allowedReturns.includes(requestedReturn) ? requestedReturn : null;
  const returnSuffix = returnTo ? '?returnTo=' + encodeURIComponent(returnTo) : '';
  const onboardingUrl = config.origin + '/__auth/onboarding' + returnSuffix;
  let ticket = incoming.searchParams.get('__clerk_ticket');
  if (ticket) history.replaceState(null, '', location.pathname);
  const show = (id) => {
    for (const section of document.querySelectorAll('[data-panel]'))
      section.hidden = section.id !== id;
  };
  const run = (action) => async () => {
    if (busy) return;
    busy = true;
    for (const button of document.querySelectorAll('button')) button.disabled = true;
    say('Please wait…');
    try {
      await action();
    } catch (error) {
      const code = error?.errors?.[0]?.code;
      say(
        code === 'form_password_pwned'
          ? 'Please choose a different password. This password has appeared in a data breach.'
          : code === 'form_password_length_too_short'
            ? 'Please use a longer password.'
            : 'That step could not be completed. Check your details and try again. If your invitation expired, ask the owner for a new one.',
      );
    } finally {
      busy = false;
      for (const button of document.querySelectorAll('button'))
        button.disabled = button.id === 'google' && config.googleEmailOnlyVerified !== true;
      for (const input of document.querySelectorAll('input[type=password]')) input.value = '';
    }
  };
  const api = async (path, body) => {
    const token = await clerk.session?.getToken();
    if (!token) throw new Error('Session unavailable');
    const response = await fetch(path, {
      method: body ? 'POST' : 'GET',
      headers: {
        Authorization: `Bearer ${token}`,
        ...(body ? { 'Content-Type': 'application/json', 'X-Ryhze-Request': 'onboarding-v1' } : {}),
      },
      ...(body ? { body: JSON.stringify(body) } : {}),
      credentials: 'same-origin',
      redirect: 'error',
    });
    const data = await response.json();
    if (!response.ok) {
      if (data.code === 'profile_conflict') {
        say('That user ID is unavailable. Try another.');
        return null;
      }
      if (data.code === 'profile_invalid') {
        say(
          'Use 1–64 letters, numbers, dots or underscores for your user ID, and a nonempty nickname up to 80 characters.',
        );
        return null;
      }
      throw new Error('Request rejected');
    }
    return data;
  };
  const resumeContent = async () => {
    if (!returnTo || !clerk.session) return false;
    try {
      const token = await clerk.session.getToken({ skipCache: true });
      const response = await fetch('/__auth/content-status', {
        headers: { Authorization: `Bearer ${token}` },
        credentials: 'same-origin',
        redirect: 'manual',
        cache: 'no-store',
      });
      if (response.status !== 204) return false;
      location.replace(returnTo);
      return true;
    } catch {
      return false;
    }
  };
  const onboarding = async () => {
    show('waiting');
    const data = await api('/__auth/session');
    if (!data) return;
    if (data.stage === 'invitation') {
      show('claim');
      say('Your identity is verified. Continue to confirm your invitation.');
    } else if (data.stage === 'profile') {
      show('profile');
      say('Choose your nickname and a unique user ID.');
    } else {
      show('ready');
      say(
        'Your account profile is ready. Tester content is waiting for approved agreements and release readiness.',
      );
      if (data.stage === 'ready-for-agreements') {
        say('Your profile is ready. Continue to review the testing agreements.');
        $('continue-preview').hidden = false;
      }
      $('saved-userid').textContent = data.profile?.userId ?? '';
      $('saved-nickname').textContent = data.profile?.nickname ?? '';
    }
    $('signout').hidden = false;
  };
  const finish = async (resource) => {
    if (resource.status === 'complete' && resource.createdSessionId) {
      await clerk.setActive({ session: resource.createdSessionId });
      if (clerk.session?.currentTask) {
        show('waiting');
        say(
          'Your account needs an additional provider security step. Complete it in the managed account portal.',
        );
        return;
      }
      history.replaceState(null, '', '/__auth/onboarding' + returnSuffix);
      ticket = null;
      if (!(await resumeContent())) await onboarding();
      return true;
    }
    return false;
  };
  $('email-submit').onclick = run(async () => {
    const emailAddress = $('email').value.trim();
    const password = $('password').value;
    if (!emailAddress || !password) {
      say('Enter your email address and password.');
      return;
    }
    const registering = $('registration').checked;
    if (registering && password !== $('confirmation').value) {
      say('The passwords do not match.');
      return;
    }
    if (registering) {
      if (!ticket && !clerk.client.signUp.id) {
        say('Open the invitation link from the owner to create an account.');
        return;
      }
      const attempt = ticket
        ? await clerk.client.signUp.create({ strategy: 'ticket', ticket, emailAddress, password })
        : await clerk.client.signUp.update({ emailAddress, password });
      ticket = null;
      if (await finish(attempt)) return;
      if (attempt.unverifiedFields.includes('email_address')) {
        await attempt.prepareEmailAddressVerification({ strategy: 'email_code' });
        verification = 'signup';
        show('verify');
        say('Enter the verification code sent to your invited email address.');
      } else {
        say('Additional account setup is required. Please check with the owner.');
      }
    } else {
      const attempt = await clerk.client.signIn.create({ identifier: emailAddress, password });
      if (await finish(attempt)) return;
      if (attempt.status === 'needs_second_factor' || attempt.status === 'needs_client_trust') {
        const emailFactor = attempt.supportedSecondFactors.find(
          (factor) => factor.strategy === 'email_code',
        );
        if (!emailFactor) {
          say(
            'This account needs another security method. Use the managed account portal to finish signing in.',
          );
          return;
        }
        await attempt.prepareSecondFactor({
          strategy: 'email_code',
          emailAddressId: emailFactor.emailAddressId,
        });
        verification = 'signin';
        show('verify');
        say('Enter the security code sent to your email.');
      } else {
        say('An additional provider security step is required before signing in.');
      }
    }
  });
  $('resend').onclick = run(async () => {
    if (verification === 'signup')
      await clerk.client.signUp.prepareEmailAddressVerification({ strategy: 'email_code' });
    else if (verification === 'signin') {
      const factor = clerk.client.signIn.supportedSecondFactors.find(
        (item) => item.strategy === 'email_code',
      );
      if (!factor) throw new Error('Email verification unavailable');
      await clerk.client.signIn.prepareSecondFactor({
        strategy: 'email_code',
        emailAddressId: factor.emailAddressId,
      });
    } else if (verification === 'reset') {
      const identifier = clerk.client.signIn.identifier || $('email').value.trim();
      if (!identifier) throw new Error('Restart password reset');
      await clerk.client.signIn.create({ strategy: 'reset_password_email_code', identifier });
    } else throw new Error('Restart verification');
    say('A new code has been requested. Check your email.');
  });
  $('verify-submit').onclick = run(async () => {
    const code = $('code').value.trim();
    let attempt;
    if (verification === 'signup')
      attempt = await clerk.client.signUp.attemptEmailAddressVerification({ code });
    else if (verification === 'signin')
      attempt = await clerk.client.signIn.attemptSecondFactor({ strategy: 'email_code', code });
    else if (verification === 'reset') {
      const password = $('new-password').value;
      if (!password || password !== $('new-confirmation').value) {
        say('Enter matching new passwords.');
        return;
      }
      attempt = await clerk.client.signIn.attemptFirstFactor({
        strategy: 'reset_password_email_code',
        code,
        password,
      });
    } else {
      say('Start the sign-in step again.');
      return;
    }
    $('code').value = '';
    if (!(await finish(attempt)))
      say('Another security step is required. Please use the managed account portal.');
  });
  $('google').onclick = run(async () => {
    if (config.googleEmailOnlyVerified !== true) {
      say('Google sign-in is paused until email-only permissions are verified.');
      return;
    }
    if ($('registration').checked) {
      if (!ticket && !clerk.client.signUp.id) {
        say('Open your invitation link first.');
        return;
      }
      if (ticket) {
        await clerk.client.signUp.create({ strategy: 'ticket', ticket });
        ticket = null;
      }
      await clerk.client.signUp.authenticateWithRedirect({
        strategy: 'oauth_google',
        continueSignUp: true,
        redirectUrl: config.origin + '/__auth/callback' + returnSuffix,
        redirectUrlComplete: onboardingUrl,
      });
    } else {
      await clerk.client.signIn.authenticateWithRedirect({
        strategy: 'oauth_google',
        redirectUrl: config.origin + '/__auth/callback' + returnSuffix,
        redirectUrlComplete: onboardingUrl,
      });
    }
  });
  $('forgot').onclick = run(async () => {
    const email = $('email').value.trim();
    if (!email) {
      say('Enter your email address first.');
      return;
    }
    await clerk.client.signIn.create({ strategy: 'reset_password_email_code', identifier: email });
    verification = 'reset';
    $('reset-fields').hidden = false;
    show('verify');
    say('If this account can reset its password, use the code sent to your email.');
  });
  $('claim-submit').onclick = run(async () => {
    await api('/__auth/redeem', {});
    await onboarding();
  });
  $('profile-submit').onclick = run(async () => {
    const body = { nickname: $('nickname').value, userId: $('userid').value };
    if (
      !/^[A-Za-z0-9._]{1,64}$/.test(body.userId) ||
      !body.nickname.trim() ||
      [...body.nickname.trim()].length > 80 ||
      /[\u0000-\u001f\u007f-\u009f]/.test(body.nickname)
    ) {
      say(
        'Use 1–64 letters, numbers, dots or underscores for your user ID, and a nonempty nickname up to 80 characters.',
      );
      return;
    }
    if (await api('/__auth/profile', body)) await onboarding();
  });
  $('continue-preview').onclick = () => location.replace(returnTo || '/');
  $('signout').onclick = run(async () => {
    await clerk.signOut();
    location.replace('/__auth/sign-in');
  });
  $('restart').onclick = run(async () => {
    clerk.client.resetSignIn();
    clerk.client.resetSignUp();
    verification = null;
    ticket = null;
    location.replace('/__auth/sign-in');
  });
  $('userid').oninput = () => {
    $('userid').value = $('userid').value.replace(/[A-Z]/g, (letter) => letter.toLowerCase());
  };
  $('registration').onchange = () => {
    $('confirm-wrap').hidden = !$('registration').checked;
    $('password').autocomplete = $('registration').checked ? 'new-password' : 'current-password';
    $('email-submit').textContent = $('registration').checked
      ? 'Create invited account'
      : 'Sign in with email';
  };
  try {
    clerk = window.Clerk;
    if (!clerk) throw new Error('Authentication SDK unavailable');
    await clerk.load({
      nonce: config.nonce,
      telemetry: { disabled: true },
      signInUrl: config.origin + '/__auth/sign-in',
      signUpUrl: config.origin + '/__auth/sign-up',
      signInForceRedirectUrl: onboardingUrl,
      signUpForceRedirectUrl: onboardingUrl,
    });
    if (location.pathname === '/__auth/callback') {
      await clerk.handleRedirectCallback({
        signInUrl: config.origin + '/__auth/sign-in',
        signUpUrl: config.origin + '/__auth/sign-up',
        signInFallbackRedirectUrl: onboardingUrl,
        signUpFallbackRedirectUrl: onboardingUrl,
        signInForceRedirectUrl: onboardingUrl,
        signUpForceRedirectUrl: onboardingUrl,
      });
      return;
    }
    if (clerk.session) {
      if (!(await resumeContent())) await onboarding();
    } else if (
      !ticket &&
      location.pathname === '/__auth/sign-up' &&
      clerk.client.signUp?.id &&
      clerk.client.signUp.unverifiedFields?.includes('email_address')
    ) {
      verification = 'signup';
      show('verify');
      say('Continue with the email verification code already sent to you.');
    } else if (
      clerk.client.signIn?.firstFactorVerification?.strategy === 'reset_password_email_code'
    ) {
      verification = 'reset';
      $('reset-fields').hidden = false;
      show('verify');
      say('Continue your password reset with the email code.');
    } else if (
      ['needs_second_factor', 'needs_client_trust'].includes(clerk.client.signIn?.status)
    ) {
      verification = 'signin';
      show('verify');
      say('Continue with the security code sent to your email.');
    } else {
      $('registration').checked = Boolean(ticket) || location.pathname === '/__auth/sign-up';
      $('registration').onchange();
      show('credentials');
      say(
        ticket
          ? 'Create your invited account with Google or email and password.'
          : 'Private testing is invitation-only. Sign in using your invited email.',
      );
    }
    for (const button of document.querySelectorAll('button'))
      button.disabled = button.id === 'google' && config.googleEmailOnlyVerified !== true;
  } catch {
    show('waiting');
    say('Secure sign-in is unavailable right now. Refresh to try again.');
  }
}
