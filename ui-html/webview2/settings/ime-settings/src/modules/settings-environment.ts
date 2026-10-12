export function applySettingsEnvironment(environment: unknown): void {
  const temporary = typeof environment === 'object' && environment !== null &&
    'temporary_configuration' in environment && environment.temporary_configuration === true;
  const notice = document.getElementById('temporary-config-notice');
  if (notice) notice.hidden = !temporary;
}
