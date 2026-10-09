import { apiFetch } from './fetch_utils'

/**
 * @brief Validate and save credentials, preserving the server's error message.
 * @param {object} passwordData Current and new credentials from the form.
 * @return {Promise<void>} Resolves after saving or rejects with a readable error.
 */
export async function saveCredentials(passwordData) {
  if (!passwordData.newPassword) {
    throw new Error('Enter a new password.')
  }
  if (passwordData.newPassword !== passwordData.confirmNewPassword) {
    throw new Error('Password Mismatch')
  }

  let response
  try {
    response = await apiFetch('/api/password', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(passwordData),
    })
  } catch {
    throw new Error('Could not connect to Sunshine. Check that it is running and try again.')
  }

  let result
  try {
    result = await response.json()
  } catch {
    throw new Error('Sunshine returned an invalid response (HTTP ' + response.status + ').')
  }
  if (response.status !== 200 || result?.status !== true) {
    throw new Error(
      typeof result?.error === 'string' && result.error
        ? result.error
        : 'Sunshine could not save the credentials (HTTP ' + response.status + ').',
    )
  }
}
