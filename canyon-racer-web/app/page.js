import Player from './player';

export default function Home() {
  return (
    <main>
      <header className="page-header">
        <div>
          <p className="eyebrow">Game Boy · 2023</p>
          <h1>Canyon <span>Racer</span></h1>
        </div>
        <a className="source-link" href="https://github.com/bbbbbr/canyon-racer">Source code <span aria-hidden="true">↗</span></a>
      </header>
      <Player />
      <footer>
        <span>Game by <a href="https://bbbbbr.itch.io/canyon-racer">bbbbbr</a> · Music and SFX by BeatScribe</span>
        <a href="/rom/LICENSE.txt">Game license</a>
      </footer>
    </main>
  );
}
