import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { flushPromises, mount } from '@vue/test-utils'

vi.mock('../../src_assets/common/assets/web/fetch_utils', () => ({ apiFetch: vi.fn() }))
vi.mock('../../src_assets/common/assets/web/Navbar.vue', () => ({ default: { template: '<div />' } }))
vi.mock('../../src_assets/common/assets/web/NavbarSimple.vue', () => ({ default: { template: '<div />' } }))
vi.mock('../../src_assets/common/assets/web/Notification.vue', () => ({ default: { template: '<div />' } }))
vi.mock('../../src_assets/common/assets/web/ResourceCard.vue', () => ({ default: { template: '<div />' } }))

import { apiFetch } from '../../src_assets/common/assets/web/fetch_utils'
import { saveCredentials } from '../../src_assets/common/assets/web/credentials'
import Welcome from '../../src_assets/common/assets/web/Welcome.vue'
import Password from '../../src_assets/common/assets/web/Password.vue'

const validCredentials = {
  newUsername: 'sunshine',
  newPassword: 'test-only-password',
  confirmNewPassword: 'test-only-password',
}

beforeEach(() => {
  vi.resetAllMocks()
  vi.useFakeTimers()
})
afterEach(() => {
  vi.clearAllTimers()
  vi.useRealTimers()
  vi.unstubAllGlobals()
})

/**
 * @brief Provide a server response without sending credentials to a real host.
 * @param {number} status HTTP response status.
 * @param {unknown} body Parsed server response.
 * @return {void}
 */
function respond(status, body) {
  apiFetch.mockResolvedValue({ status, json: async () => body })
}

describe('credential response handling', () => {
  it('does not send mismatched passwords to the server', async () => {
    await expect(saveCredentials({ ...validCredentials, confirmNewPassword: 'different' }))
      .rejects.toThrow('Password Mismatch')
    expect(apiFetch).not.toHaveBeenCalled()
  })

  it('rejects an empty password without changing credentials', async () => {
    await expect(saveCredentials({ ...validCredentials, newPassword: '', confirmNewPassword: '' }))
      .rejects.toThrow('Enter a new password.')
    expect(apiFetch).not.toHaveBeenCalled()
  })

  it('posts JSON to the root endpoint even from /welcome/', async () => {
    respond(200, { status: true })
    await expect(saveCredentials(validCredentials)).resolves.toBeUndefined()
    expect(apiFetch).toHaveBeenCalledWith('/api/password', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(validCredentials),
    })
  })

  it.each([
    [400, 'Password Mismatch'],
    [400, 'Missing CSRF token'],
    [500, 'Could not write credentials'],
    [200, 'Invalid Username'],
  ])('shows the server error for HTTP %s: %s', async (status, error) => {
    respond(status, { status: false, error })
    await expect(saveCredentials(validCredentials)).rejects.toThrow(error)
  })

  it.each([undefined, null, {}, { status: false }, { error: '' }, { error: 1 }, { status: true }])
    ('reports the HTTP status when a failure has no readable server message: %j', async body => {
      respond(403, body)
      await expect(saveCredentials(validCredentials)).rejects.toThrow('HTTP 403')
    })

  it('requires an explicit successful result from HTTP 200', async () => {
    respond(200, { status: 'true' })
    await expect(saveCredentials(validCredentials)).rejects.toThrow('HTTP 200')
  })

  it('handles a non-JSON error response', async () => {
    apiFetch.mockResolvedValue({ status: 500, json: async () => { throw new SyntaxError('Not JSON') } })
    await expect(saveCredentials(validCredentials)).rejects.toThrow('invalid response (HTTP 500)')
  })

  it('gives a readable error when Sunshine cannot be reached', async () => {
    apiFetch.mockRejectedValue(new TypeError('Failed to fetch'))
    await expect(saveCredentials(validCredentials)).rejects.toThrow('Could not connect to Sunshine')
  })
})

describe.each([['initial setup', Welcome], ['password change', Password]])('%s form', (_name, component) => {
  it('renders the actual server message and re-enables submission', async () => {
    let resolveRequest
    apiFetch.mockImplementation(() => new Promise(resolve => { resolveRequest = resolve }))
    const wrapper = mount(component, { global: { mocks: { $t: key => key } } })
    await wrapper.setData({ passwordData: validCredentials })
    await wrapper.find('form').trigger('submit')
    expect(wrapper.find('button[type="submit"]').element.disabled).toBe(true)
    resolveRequest({ status: 400, json: async () => ({ status: false, error: 'Invalid Username' }) })
    await flushPromises()
    expect(wrapper.find('.alert-danger').text()).toContain('Invalid Username')
    expect(wrapper.find('button[type="submit"]').element.disabled).toBe(false)
    wrapper.unmount()
  })

  it('displays the actual validation error and clears stale success', async () => {
    const form = { ...component.data(), passwordData: validCredentials, success: true }
    respond(400, { status: false, error: 'Invalid Username' })
    await component.methods.save.call(form)
    expect(form.error).toBe('Invalid Username')
    expect(form.success).toBe(false)
    expect(form.loading).toBe(false)
    expect(vi.getTimerCount()).toBe(0)
  })

  it('allows another attempt after a connection failure', async () => {
    const form = { ...component.data(), passwordData: validCredentials }
    apiFetch.mockRejectedValue(new TypeError('Failed to fetch'))
    await component.methods.save.call(form)
    expect(form.loading).toBe(false)
    expect(form.error).toContain('Could not connect to Sunshine')
    respond(200, { status: true })
    await component.methods.save.call(form)
    expect(form.error).toBeNull()
    expect(form.success).toBe(true)
  })

  it('keeps the form busy until the response arrives', async () => {
    let resolveRequest
    apiFetch.mockImplementation(() => new Promise(resolve => { resolveRequest = resolve }))
    const form = { ...component.data(), passwordData: validCredentials }
    const submission = component.methods.save.call(form)
    expect(form.loading).toBe(true)
    resolveRequest({ status: 200, json: async () => ({ status: true }) })
    await submission
    expect(form.loading).toBe(false)
  })

  it('reloads after five seconds only following successful setup', async () => {
    const reload = vi.fn()
    vi.stubGlobal('document', { location: { reload } })
    const form = { ...component.data(), passwordData: validCredentials, error: 'previous error' }
    respond(200, { status: true })
    await component.methods.save.call(form)
    expect(form.success).toBe(true)
    expect(form.error).toBeNull()
    await vi.advanceTimersByTimeAsync(4999)
    expect(reload).not.toHaveBeenCalled()
    await vi.advanceTimersByTimeAsync(1)
    expect(reload).toHaveBeenCalledOnce()
  })
})
