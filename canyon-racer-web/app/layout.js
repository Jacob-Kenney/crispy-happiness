import '@fontsource/archivo-black/latin-400.css';
import '@fontsource/ibm-plex-mono/latin-400.css';
import '@fontsource/instrument-sans/latin-400.css';
import '@fontsource/instrument-sans/latin-600.css';
import './globals.css';

export const metadata = {
  title: 'Canyon Racer',
  description: 'Play the original Canyon Racer Game Boy demo in your browser.',
};

export default function RootLayout({ children }) {
  return <html lang="en"><body>{children}</body></html>;
}
