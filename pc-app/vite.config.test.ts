import { expect, test } from 'vitest'
import config from './vite.config.ts'

test('base path matches the GitHub Pages sub-path', () => {
  expect(config.base).toBe('/esp32S3_cam/')
})
