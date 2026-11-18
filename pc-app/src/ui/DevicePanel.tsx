import type { DeviceInfo } from '../protocol/messages'
import type { ConnectionStatus } from '../serial/connection'
import { strings, type ErrorText } from '../strings'

interface Props {
  status: ConnectionStatus
  /** True when the last disconnect was not requested by the user. */
  lost: boolean
  info: DeviceInfo | null
  error: ErrorText | null
  onConnect(): void
  onDisconnect(): void
}

function statusText(status: ConnectionStatus, lost: boolean): string {
  if (status === 'connecting') return strings.statusConnecting
  if (status === 'connected') return strings.statusConnected
  return lost ? strings.statusLost : strings.statusDisconnected
}

export default function DevicePanel({ status, lost, info, error, onConnect, onDisconnect }: Props) {
  const busy = status === 'connecting' || status === 'connected'
  return (
    <div>
      <p>
        <button type="button" onClick={onConnect} disabled={busy}>
          {strings.connect}
        </button>{' '}
        <button type="button" onClick={onDisconnect} disabled={!busy}>
          {strings.disconnect}
        </button>
      </p>
      <p>{statusText(status, lost)}</p>
      {error && (
        <p role="alert">
          {strings.error}: {error.message}
          {error.detail && (
            <>
              {' '}
              <small>{error.detail}</small>
            </>
          )}
        </p>
      )}
      {info && (
        <dl>
          <dt>{strings.protocolVersion}</dt>
          <dd>{info.protocolVersion}</dd>
          <dt>{strings.firmware}</dt>
          <dd>
            {info.firmwareVersion} ({info.firmwareGit})
          </dd>
          <dt>{strings.serialNumber}</dt>
          <dd>{info.serial}</dd>
          <dt>{strings.frameSize}</dt>
          <dd>
            {info.frameWidth}x{info.frameHeight}
          </dd>
          <dt>{strings.maxResults}</dt>
          <dd>{info.maxResults}</dd>
          <dt>{strings.functions}</dt>
          <dd>{info.functionIds.join(', ')}</dd>
        </dl>
      )}
    </div>
  )
}
