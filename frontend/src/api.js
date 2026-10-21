export async function api(path, { method = 'GET', body } = {}) {
  const response = await fetch(path, { method, credentials: 'include', headers: body ? { 'Content-Type': 'application/json' } : undefined, body: body ? JSON.stringify(body) : undefined });
  const data = await response.json().catch(() => ({}));
  return { ok: response.ok, status: response.status, data };
}
