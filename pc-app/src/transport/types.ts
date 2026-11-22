// The byte pipe under MxcamConnection. The MX protocol (docs/protocol.md) is the same on every transport;
// only how the bytes travel differs.

export type TransportKind = 'usb' | 'bluetooth'

/** An open link to one MXCam. */
export interface ByteTransport {
  readonly kind: TransportKind
  /** Device-to-host bytes in arrival order; chunk boundaries mean nothing. */
  readonly readable: ReadableStream<Uint8Array>
  /** Host-to-device bytes. */
  readonly writable: WritableStream<Uint8Array>
  /** Registers a callback for the link dropping on its own (unplugged, out of range). Not called after close(). */
  onLost(cb: () => void): void
  /** Releases the link. Call after the reader and writer are released. */
  close(): Promise<void>
}

/** A device that was found but is not open yet. */
export interface TransportCandidate {
  readonly kind: TransportKind
  /** Opens the link. When it rejects, nothing is left open. */
  open(): Promise<ByteTransport>
}

/** Finds devices of one kind. */
export interface TransportProvider {
  readonly kind: TransportKind
  /** Lets the user pick a device (needs a user gesture). Resolves to null when the user closes the picker. */
  request(): Promise<TransportCandidate | null>
  /** Devices that can be opened without asking the user again. Empty for kinds that always need a gesture. */
  granted(): Promise<TransportCandidate[]>
  /**
   * Calls `cb` when a device of this kind appears (e.g. plugged in): with the candidate when it is known to be
   * an MXCam, with undefined otherwise. Returns the unsubscribe function.
   */
  onAttach(cb: (candidate: TransportCandidate | undefined) => void): () => void
}
