<?hh // strict

function is_vec_(mixed $x): void {
  if ($x is vec<_>) {
    echo "vec\n";
  } else {
    echo "not vec\n";
  }
}

function is_dict_(mixed $x): void {
  if ($x is dict<_, _>) {
    echo "dict\n";
  } else {
    echo "not dict\n";
  }
}


function is_keyset_(mixed $x): void {
  if ($x is keyset<_>) {
    echo "keyset\n";
  } else {
    echo "not keyset\n";
  }
}

is_vec_(1);
is_vec_(true);
is_vec_(1.5);
is_vec_('foo');
is_vec_(null);
is_vec_(STDIN);
is_vec_(array());
is_vec_(vec[]);
is_vec_(dict[]);
is_vec_(keyset[]);

echo "---\n";

is_dict_(1);
is_dict_(true);
is_dict_(1.5);
is_dict_('foo');
is_dict_(null);
is_dict_(STDIN);
is_dict_(array());
is_dict_(vec[]);
is_dict_(dict[]);
is_dict_(keyset[]);

echo "---\n";

is_keyset_(1);
is_keyset_(true);
is_keyset_(1.5);
is_keyset_('foo');
is_keyset_(null);
is_keyset_(STDIN);
is_keyset_(array());
is_keyset_(vec[]);
is_keyset_(dict[]);
is_keyset_(keyset[]);